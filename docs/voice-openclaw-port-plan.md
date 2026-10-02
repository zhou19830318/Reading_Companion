# Voice STT + OpenClaw Port Plan (AIWatch_Ver2.0 → OnePage/ESP32-C61)

Status: **P0, P1 verified** (record → HTTPS POST → SSE transcript). **P3 text round-trip verified**
(WS authenticated, `chat.send` → clean `reply: PONG` with gateway failure markers stripped, sessionKey
`agent:main:crosspoint`). **P3-S Phase 1 verified** — voice conversation → on-screen LLM reply, with
history persistence and auto/manual modes (§10.5/§10.7); **UI closeout + the "stuck waiting for reply"
fixes are in §11, device-verified 2026-09-28** including a full tool-call round (alarm clock, 10.6 s) and
the M0 fixes for gateway-frame repaints and bare-markdown replies (§11.5).
P2 open: internal DRAM is still tight with WiFi up — see §4 P2 for why the
sdkconfig route does not work on this toolchain.
Source project: `/home/conor/esp32/AIWatch_Ver2.0` (upstream: ESP32-**S3**, 8MB PSRAM)
Target project: this repo (`onepage`, ESP32-**C61**, 2MB PSRAM)

This document is the pre-implementation preparation: pitfalls, both codebases,
the staged plan, and the verification method for each stage.

---

## 1. What is actually being ported

| Piece | Source file(s) | What it does | Kind |
| --- | --- | --- | --- |
| STT | `components/stt/stt_client.c` (551 L) | Record PCM → wrap in 44-byte WAV → base64 → single HTTPS POST; parse SSE reply | **Cloud** HTTP REST |
| OpenClaw | `components/openclaw/openclaw_client.c` (2500 L) | WebSocket to OpenClaw Gateway; Ed25519 device identity; chat / stream / audio / image / cron / usage | **Cloud** WebSocket |
| Orchestration | `main/voice_chat.c` (434 L) | record → local VAD → STT → send → stream reply; LVGL UI | App logic |
| Audio capture | `components/board/board.c:863` | `board_audio_record()` = I2S **STD** + ES7210 ADC, 16 kHz | Board HAL |
| Wake word | `components/wake_word` | esp-sr WakeNet `nihaoxiaozhi` | **Not portable** (esp-sr has no C61 support) |

Key facts verified from source:
- STT is **server-side ASR** (Xiaomi MiMo `mimo-v2.5-asr`, OpenAI-compatible
  `/v1/chat/completions`, `api-key` header). **No on-device model.** This is why
  the port is viable at all — it sidesteps the C61's lack of SIMD / esp-sr.
- OpenClaw client is **custom** (not the official `esp-openclaw-node`): it uses
  `esp_websocket_client` (`openclaw_client.c:18`), `ed25519.h` (`:20`),
  `mbedtls/sha256.h` (`:23`), signs `connect.challenge` via `ed25519_sign` (`:423`),
  and supports `wss://` with `esp_crt_bundle` (`:1391-1393`).
- STT memory note in source: *"Peak usage is ~2 MB during finalize … which is
  safe on 8MB PSRAM systems."* Accumulator caps at 30 s (`ACCUM_MAX_SAMPLES=480000`).

---

## 2. Pitfalls / risks (researched)

### 2.1 PDM mic — no hardware PDM→PCM on C61 (highest-impact)

Verified in the installed ESP-IDF (`components/soc/esp32c61/include/soc/soc_caps.h`):

```
SOC_I2S_SUPPORTS_PDM      (1)
SOC_I2S_SUPPORTS_PDM_TX   (1)   // raw PDM out
SOC_I2S_SUPPORTS_PCM2PDM  (1)   // TX: PCM->PDM hardware filter
SOC_I2S_SUPPORTS_PDM_RX   (1)   // raw PDM in
# SOC_I2S_SUPPORTS_PDM2PCM  -> ABSENT
```

Consequence (`esp_driver_i2s/include/driver/i2s_pdm.h:89-92`):

```c
#if SOC_I2S_SUPPORTS_PDM2PCM
#  define I2S_PDM_RX_SLOT_DEFAULT_CONFIG(...) I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(...)
#else
#  define I2S_PDM_RX_SLOT_DEFAULT_CONFIG(...) I2S_PDM_RX_SLOT_RAW_FMT_DEFAULT_CONFIG(...)
#endif
```

So on C61 the I2S peripheral can only read **raw PDM**. The PDM→PCM conversion
(low-pass + decimate + high-pass + gain) must be done **in software**.
This is the single biggest addition vs. the S3 source (where the board used an
I2S/ES7210 path).

### 2.2 Mic power rail is shared

`lib/hal/HalGPIO.h:26-27` — mic is on `PDM_CLK 7` / `PDM_DIN 3`, and its power
rail is `GPIO27` = `EPD_RST`, **shared with SD + EPD** (`HalGPIO.cpp:282`).
- Cannot cut the rail while recording.
- The EPD hardware reset pulse browns out the SD (`HalGPIO.cpp:209`). Recording
  must not overlap an EPD reset; keep capture isolated from display reset.
- Mic capture path is **currently unproven** — firmware only silences it.
  That is why P0 exists.

### 2.3 PDM details
- Raw PDM RX words are always 16-bit; mono = 1 line = 16 PDM bits/word.
- PDM `sample_rate_hz` is the **PDM clock** (typ. 1.024 MHz for 16 kHz @ OS=64),
  not the PCM rate.
- PDM mics have DC bias → high-pass/DC-block required (AIWatch's `voice_chat.c`
  does an explicit DC-offset calibration).
- The mic's `LR`/`SEL` pin is usually hard-wired; not driven by the ESP.

### 2.4 Memory (the real ceiling)
Measured on this repo (`pio run -e onepage`): Flash 85.7% used (~937 KB free of
the 6.25 MB app slot); **DIRAM 89.98% (≈25.7 KB link-time headroom)**; PSRAM 2 MB
total.

| Need | Cost | Notes |
| --- | --- | --- |
| I2S RX DMA buffers | small, **internal DMA-capable** | competes with the 25.7 KB |
| STT accumulate + WAV + base64 + JSON | **0.7–2 MB, PSRAM** | must cap duration / stream |
| WebSocket client + mbedTLS session + cert bundle | **~40–80 KB, internal** | the binding constraint |
| esp_crt_bundle | tens of KB flash | |

### 2.5 TLS / time / crypto
- `esp_crt_bundle` validates cert dates → **system clock must be correct (SNTP)**
  before any HTTPS/WSS. This device has no RTC.
- AIWatch **disabled hardware AES** because the AES accelerator needs
  DMA-capable internal DRAM and their allocations failed
  (`sdkconfig.defaults`: *"Disable AES hardware acceleration …"*). Same class of
  problem exists on C61; may need `CONFIG_MBEDTLS_HARDWARE_AES=n`.
- AIWatch frees internal DRAM with `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`,
  `CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y`, `CONFIG_SPIRAM_RODATA=y`. This repo has
  none of them. **Do not blanket-copy** — PSRAM here is only 40 MHz, so
  `FETCH_INSTRUCTIONS/RODATA` hurts code fetch speed.

### 2.6 Project-structure pitfalls
- **Simulator mirrors HAL 1:1.** `simulator/CMakeLists.txt` excludes
  `lib/hal/Hal*.cpp` (`list(FILTER ... REGEX "lib/hal/Hal.*\\.cpp$")`) and links
  `simulator/hal/Hal*.cpp` instead. A new `lib/hal/HalMicrophone.cpp` is
  auto-excluded → a matching `simulator/hal/HalMicrophone.cpp` **must** be added
  to `SIMULATOR_HAL` or the sim link breaks.
- Activity lifecycle: activities are heap-allocated and `delete`d on exit; any
  buffer/task allocated in `onEnter()` must be released in `onExit()`.
- `-fno-exceptions` → **never bare `new`**; use `makeUniqueNoThrow<T>()`.
- User-facing text must use `tr()`; log text may be raw. Adding strings means
  editing `lib/I18n/translations/english.yaml` (reference; others fall back).
- No RTTI, no `std::string` in hot paths; render through `UITheme`/`GUI`.
- Flash is 85.7% full and OTA needs both app slots — any large addition shrinks
  the OTA margin.
- `lib_deps` has `links2004/WebSockets`, **not** `esp_websocket_client`.
- The project already has HTTPS client + mbedTLS + cJSON-equivalent
  (ArduinoJson) + WiFi STA — reuse instead of new deps.

### 2.7 Scope conflict
`SCOPE.md` lists *Media Playback* and *Active Connectivity* as out-of-scope, and
warns background Wi-Fi drains battery. A voice assistant touches both. Frame it
as an **opt-in, user-initiated** feature (button-press, single round-trip), not a
resident background assistant. e-ink cannot do real-time streaming UI.

---

## 3. Target architecture in this repo

```
                 (user presses button)
  MicrophoneTestActivity / future VoiceActivity
        |                          |
        v                          v
  HalMicrophone  ──PCM──>  SttClient (cloud ASR)  ──text──>  OpenClawClient (WS)
   (PDM RX +                esp_http_client +               esp_websocket_client
    SW PDM→PCM)             mbedtls + crt_bundle            + ed25519 + JSON
```

Layering follows the HAL skill: hardware behind `Hal*`, cloud clients as plain
libs under `lib/`, UI as Activities. STT and OpenClaw clients are **PCM/text in,
text out** and therefore board-agnostic (same shape as the source).

---

## 4. Staged plan + verification per stage

### P0 — Prove the PDM microphone (this stage)
Deliverables:
- `lib/hal/HalMicrophone.{h,cpp}` — I2S raw-PDM RX + software PDM→PCM
  (CIC decimate → DC blocker → gain), 16 kHz mono int16 out.
- `simulator/hal/HalMicrophone.cpp` — stub (returns unavailable).
- `src/activities/settings/MicrophoneTestActivity.{h,cpp}` — record N seconds,
  show RMS/peak/DC on e-ink, write `/mic_test.wav` to SD.
- Settings-menu entry (`SettingAction::MicrophoneTest`), i18n strings.

**Verification**
1. Build: `pio run -e onepage` → 0 errors.
2. Device: Settings → System → Microphone Test → Confirm.
   - PASS: RMS rises clearly when speaking, near noise floor when silent;
     `/mic_test.wav` plays back as intelligible speech.
   - FAIL modes to distinguish: all-zero/flat → mic not powered, wrong pin, or
     LR not tied; constant loud buzz → PDM clock wrong; muffled → expected
     (simple filter), acceptable.
3. Heap: log internal + PSRAM free before/after capture; confirm DMA buffers
   released on exit (`MIC.end()` + activity `onExit()`).

### P1 — Cloud STT, no OpenClaw yet
- `lib/voice/SttClient.{h,cpp}` — port `stt_client.c`, but: cap duration
  (≈8–10 s default); build JSON with **ArduinoJson** (drop the cJSON dep);
  reuse existing `esp_http_client` + `esp_crt_bundle`; keep buffers in PSRAM via
  `makeUniqueNoThrow`.
- `src/activities/voice/VoiceInputActivity` — button → record → "Transcribing…"
  → show transcript on e-ink.
- Prereq: SNTP sync before POST; add settings for API key + endpoint.

**Verification**: unit-test the WAV builder + JSON body offline; then on device
record a short phrase and compare transcript; log HTTP status + response bytes.
Reject-empty-text path must not crash.

### P2 — Free internal DRAM
- Evaluate `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`,
  `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`, `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`,
  possibly `CONFIG_MBEDTLS_HARDWARE_AES=n`, `CONFIG_LWIP_MAX_SOCKETS=16`.
- Measure before/after internally and re-run the full regression (EPUB read,
  OTA check, BLE page-turner, Wi-Fi transfer).

**Verification**: `pio run -e onepage` size table; on-device
`heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`; regression checklist.

**Result of the first attempt (measured, do not repeat blindly).** pioarduino
links *prebuilt* IDF archives and the mbedtls / lwip / esp_wifi sources the
build compiles never reach the link line (`firmware.map`: 0 entries under
`.pio/build/onepage/mbedtls/`, `esp_mbedtls_mem_calloc` ←
`libmbedcrypto.a(esp_mem.c.obj)`, `mbedtls_ssl_setup` ←
`libmbedtls_2.a(ssl_tls.c.obj)`). So:

- `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` — **inert** (archive built with the
  default INTERNAL policy).
- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` — **inert** (`liblwip.a`,
  `libesp_wifi.a`, `libnet80211.a` are prebuilt).
- `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` — dropped by kconfig: it depends on
  `!MBEDTLS_SSL_PROTO_DTLS` and `CONFIG_MBEDTLS_SSL_PROTO_DTLS=y`.
- `CONFIG_MBEDTLS_HARDWARE_AES=n` — not needed: the AES DMA path allocates with
  `heap_caps(..., MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)` directly
  (`esp_aes_dma_core.c:455`), bypassing the platform allocator.

The flags are left in `platformio.ini` with a comment explaining this; the fix
that actually works is the runtime allocator override in `lib/voice/SttClient.cpp`
(§6). P2 remains open as "free internal DRAM" — with WiFi up the internal heap
is still Free ~9.6 KB / MaxAlloc ~4.8 KB / Min Free ~2.4 KB.

**Further measurements (do not regress).**

- `SdCardFont`'s on-demand glyph bitmap ring (`OVERFLOW_CAPACITY = 128` slots)
  allocated with `new[]` in internal DRAM. A Chinese UI fills it in a couple of
  page turns, costing ~38 KB — enough to drop free heap to `270 B` right before
  `WiFi.begin()`, which is what made WiFi fail to start. Routed through
  `allocOverflowBitmap()` / `freeOverflowBitmap()` (PSRAM first, `new[]`
  fallback, released via `heap_caps_free`). Measured: internal heap holds at
  ~41.5 KB across 400 overflow loads.
- `heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL)` is a
  **no-op here**: `malloc(5001)` lands in PSRAM both before and after the call.
  Removed rather than left as dead code.
- WiFi still costs ~38 KB of internal heap on `WiFi.mode(WIFI_STA)` +
  `WiFi.begin()` (Free `46350 → 8382`). That is the working budget: anything
  else running at that moment has to fit in the remainder.

### P3 — OpenClaw client
- Vendor `ed25519_lib`; add `esp_websocket_client` (or adapt to the existing
  `links2004/WebSockets`); port `openclaw_client.c`, decoupling it from AIWatch's
  `settings`/`notes_manager`.
- Start with `ws://` to prove the `connect.challenge` + signature handshake,
  then `wss://`.

**Verification**: handshake against a local gateway; log challenge/signature
exchange; single text round-trip; then single voice round-trip.

**Result of S1 (connection spike, measured).** The connection is proven end to
end; the handshake is not yet answered.

- Endpoint confirmed: `ws://118.25.20.250:18789/` — plain `ws`, port `18789`,
  path `/`, subprotocol `v2.openclaw.io`, `tls=0`. Matches AIWatch's defaults;
  the config the device had saved was already right.
- `links2004/WebSockets` completes the upgrade and the **server speaks first**:
  ```
  [OCW] WS connected: /
  [OCW] WS text (122 bytes): {"type":"event","event":"connect.challenge","payload":{"nonce":...,"ts":...}}
  ```
  That is the S1 acceptance criterion (`WS connected` + first frame containing
  `challenge`), observed 3× on the on-device activity and 1× from the web probe.
- The server drops the link 3–15 s later: `[OCW] failed: Connection lost`.
  Nothing is sent back after the challenge, so this is expected for S1 — it is
  the S2 work item (challenge response + token auth).
- Diagnostic endpoint: `POST /api/openclaw/test` ("Test connection" on the
  Settings page) runs the same connect path but reports staged failures —
  `config | network | clock | ca | tcp | connect | ws | ok` — and returns
  `{"ok","stage","detail","firstFrame","url","ms"}`. Typical pass: 140–170 ms.
  It echoes the raw first frame, so protocol work can be driven from the
  browser without a serial capture. It reads the stored config as defaults,
  accepts an override body, and never writes NVS.
- `POST /api/cloud` used to return `500 Failed to write to NVS`: `putStr()`
  treated a `Preferences::remove()` on an absent key as failure, and `caPath` /
  `sttUrl` start empty, so every save failed after already committing the other
  keys. Now removal of an already-absent key counts as success.
- Heap guard: connect attempts must be made before the heap fragments. One
  observed `abort()` (bare `new` on OOM, `-fno-exceptions`) landed in
  `MicrophoneTest` with `Free 2570 / MaxAlloc 2420`. Panic report is written to
  `/crash_report.txt` on the SD card. Treat `MaxAlloc < 4 KB` as a hard stop
  for starting a socket.

**S2 — handshake (implemented, build-verified, device result pending).**

- `lib/ed25519/` vendors orlp/ed25519 (`keypair/sign/verify` + `fe/ge/sc/sha512`,
  `precomp_data.h` is the bulk of the flash cost). Excluded from
  `bin/clang-format-fix` alongside `lib/uzlib/` so vendor syncs stay clean.
- `lib/OpenClaw/OpenClawHandshake.{h,cpp}` is the state machine:
  `Idle → WaitChallenge → ConnectSent → Authenticated | Failed`. It owns no
  sockets — callers feed it inbound text and drain `pendingFrame()`. Pure C++
  with no Arduino dependency, so it is host-tested.
- Signature payload is byte-for-byte AIWatch's:
  `v2|<deviceId>|cli|cli|operator|operator.read,operator.write,operator.admin|<signedAtMs>|<token>|<nonce>`
  (`AIWatch .../openclaw_client.c:415`). `deviceId` = SHA-256 of the 32-byte
  public key, hex. `userAgent`/`client.id` are deliberately `AIWatch/0.5.0` /
  `cli` for S2: mirroring the known-good client removes "unknown client" as a
  variable. Flip to CrossPoint and re-run once the gateway accepts us.
- Frames are built from a member `payload_[512]` so `ed25519_sign` (measured
  ~1.3 KB of frames: `aslide[256]+bslide[256]` + sha512 context) is the only
  deep stack user. `parseFrame()` runs and returns *before*
  `buildConnectFrame()` so the two never stack.
- `lib/OpenClaw/OpenClawIdentity.{h,cpp}` (device-only): seed → SHA-256 (mbedtls
  streaming) → `ed25519_create_keypair`, plus `generateSeedHex()` via
  `esp_fill_random`. The seed is persisted once through
  `CloudConfig::setDeviceKey()` (`oc_devkey`); the gateway-issued device token
  lands in `oc_dvtok` and is written from `loop()`, never from inside the
  WebSocket callback.
- `POST /api/openclaw/test` now also exercises auth: stage list grew to
  `config | network | clock | ca | tcp | connect | ws | auth`. It reads the
  stored credentials (an unsaved token may be supplied in the body, same rule
  as `POST /api/cloud`), uses an ephemeral key only when none is stored, still
  never writes NVS, and is bounded at ~17 s. It logs the loop-task stack
  watermark — that number, not the heap, decides whether the sign path fits.
- Acceptance for S2: `stage=ok` with `authenticated as <64-hex deviceId>`, or a
  named gateway error (`AUTH_TOKEN_MISMATCH` / `NOT_PAIRED` / `missing scope:
  operator.read`) reported through `stage=auth`.

**S2 result (measured, 2026-09-26).** Three gateway rejections were walked
through in order — each one is a layer the protocol checks before the next:

1. `INVALID_REQUEST: origin not allowed` — `links2004/WebSockets` sends
   `Origin: file://` by default (`WebSocketsClient.cpp:32`). A headless client
   must send no Origin at all, which is what `esp_websocket_client` does; fixed
   with `setExtraHeaders("")` in both the activity and the probe.
2. `INVALID_REQUEST: unauthorized: gateway token mismatch` — the stored
   `oc_token` did not equal the gateway's `gateway.auth.token`. The file-token
   override path (`POST /api/openclaw/test` accepts `token`) proved our side
   was byte-exact: 49 chars, `[A-Za-z0-9_-]`, no JSON escaping needed. The
   drift was server side (config vs running service — the usual
   `gateway.remote.token` / `gateway.auth.token` split).
3. `NOT_PAIRED: pairing required: device is not approved yet` with
   `error.details.requestId` — **the protocol now reaches the pairing gate.**
   ```
   {"stage":"auth","detail":"NOT_PAIRED: pairing required: device is not approved yet",
    "requestId":"e838a478-0726-4591-9a9c-7afcf90a99ff","ms":206}
   ```
   The id is useless in a log line, so it is surfaced everywhere the failure
   is shown: `OpenClawActivity` prints a word-wrapped
   `openclaw devices approve <id>` under the error (bold, through
   `renderer.wrappedText` so a 480 px landscape panel does not run it off the
   edge), and the web test result box shows the same command.

Acceptance so far: origin + shared token + ED25519 signature are all accepted
by the gateway (a signature failure would not report `NOT_PAIRED`). Remaining
before S2 closes: approve the pending id from a loopback context on the
gateway host, then re-run the probe for `stage=ok`.

### P4 — Trim
- Keep chat send + stream + notify. Drop cron / widgets / MP3 list / image where
  not wanted, to cut flash + DRAM.

---

## 5. P0 design notes (implemented next)

Software PDM→PCM (C61 has no hardware converter):
- I2S PDM RX, **raw** format, mono (1 line), PDM clock = `SAMPLE_RATE * 64`
  = 1.024 MHz.
- Each `uint16` = 16 raw PDM bits → ±1.
- **CIC decimator** N=3, R=64 (cheap, coefficient-free anti-alias) → 16 kHz.
- **One-pole DC blocker** (high-pass) to remove the PDM mic bias.
- Gain to int16.

Tunable constants are exposed as `static constexpr` so the filter can be tuned
once real hardware data is available. Filter history lives in the HAL object
(no heap), so `read()` is allocation-free.

---

## 6. P1 implementation notes (as built)

Recorded here because they differ from §4's P1 bullets. Deviations are deliberate;
the plan text above stays as written so the reasoning is visible.

**Codec is pure, transport is device-only.** `lib/voice/` holds two layers:

| File | Depends on | Host-tested |
| --- | --- | --- |
| `SttCodec.{h,cpp}` | `<memory>`, `<cstring>`, `StreamingJsonParser` — no Arduino/ESP-IDF | yes (`test/stt_codec`) |
| `SttClient.{h,cpp}` | `esp_http_client`, `esp_crt_bundle`, `HalStorage` | no (device only) |

- **No ArduinoJson.** §4 said "build JSON with ArduinoJson". The body is fixed
  shape, so it is assembled from two literal fragments (`writeBodyHead` /
  `writeBodyTail`) plus base64 — no JSON library, no DOM, and `requestBodySize()`
  gives the exact `Content-Length` up front. The reply is parsed by the existing
  SAX `StreamingJsonParser` inside `SseTranscriptParser`.

- **Streaming upload, not a one-shot body.** A 5 s capture is 160 KB PCM →
  215 KB base64 → ~216 KB JSON. Instead of holding that, `WavBase64Writer`
  emits the base64 in chunks and the body is written to the socket as
  `head | chunks | tail`, all sized by `requestBodySize()`. Working set drops
  from ~216 KB to 28 KB (`WavBase64Writer`, one allocation, above
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` so it lands in PSRAM) + a 2 KB read
  buffer.

- **Chunk alignment.** Non-final chunks are a whole number of 3-byte input
  groups, so concatenating them equals a single `base64Encode()` of the whole
  stream — covered by `WavBase64Writer.ProducesIdenticalRequestBody`.

- **UI: no `VoiceInputActivity` yet.** §4 calls for
  `src/activities/voice/VoiceInputActivity`. The STT round-trip is wired into
  `MicrophoneTestActivity` instead: Done → *Transcribe* → transcript, with
  *Transcribe* also serving as retry. That screen already owns the PCM buffer,
  is the P0 bring-up surface, and keeps the activity surface flat. Extract a
  dedicated `VoiceInputActivity` from it once the round-trip is proven.

- **API key / endpoint are SD files, not settings entries.** §4 said "add
  settings for API key + endpoint". As built:
  - `/stt_api_key.txt` — required, one line, becomes the `api-key` header.
  - `/stt_endpoint.txt` — optional full POST URL; exists so a plain-`http`
    local server can exercise the transport without TLS, a valid clock or real
    credentials.

  Rationale: a secret must never reach the repo or the firmware image, and
  `settings.json` is written/read as a whole by the settings UI — an unknown
  secret field there invites a lossy re-save. No new settings screen either.

- **Clock prerequisite.** `esp_crt_bundle` validates certificate dates and this
  board has no RTC — every app-level NTP path (`HalClock::syncFromNTP`,
  `WifiSelectionActivity`, `ClockSyncActivity`) is gated on
  `halClock.isAvailable()`, which is false on OnePage, so the clock stays at the
  epoch forever. `SttClient::ensureSystemTime()` now does a bounded (5 s)
  `configTzTime()` sync on the first `https://` request and returns
  `Result::ERR_TIME` (screen: `STR_STT_NO_CLOCK`) if it cannot be fixed.

**Blockers found only on hardware (host tests cannot see them):**

1. **Clock.** First real POST: `system clock unset` → `ESP_ERR_HTTP_CONNECT`.
   Fixed by `ensureSystemTime()` above; verified `clock set: epoch 1790325595`.
2. **TLS OOM.** With WiFi up the internal heap has MaxAlloc 4852 B while
   `mbedtls_ssl_setup` needs two ~16.5 KB in/out buffers →
   `mbedtls_ssl_setup returned -0x7F00` (`MBEDTLS_ERR_SSL_ALLOC_FAILED`,
   `ssl.h:109`) → `ESP_ERR_HTTP_CONNECT`. The P2 sdkconfig route does not work
   on this toolchain (see §4 P2), so `SttClient` installs its own allocator with
   `mbedtls_platform_set_calloc_free()`: PSRAM first, internal fallback, for
   every size. This is the same policy as Espressif's own
   `MBEDTLS_EXTERNAL_MEM_ALLOC` (`components/mbedtls/port/esp_mem.c:18`), so it
   is a supported pairing with the hardware SHA/ECC accelerators; nothing in the
   link overwrites the setter (no archive references it and
   `mbedtls_platform_setup()` is compiled as `return 0`).
   A size-threshold version was tried first and still failed:
   `-0x4290` = `MBEDTLS_ERR_RSA_PUBLIC_FAILED` (-0x4280) **+**
   `MBEDTLS_ERR_MPI_ALLOC_FAILED` (-0x0010) — an RSA-2048 verify bignum below
   the threshold hitting internal MaxAlloc 3700 B / Min Free 483 B.

**Bugs the host tests caught before they reached hardware:**

1. `buildWavHeader()` computed `byteRate` / `blockAlign` from `bitsPerSample`
   *before* assigning it (still 0 from `memset`), so every WAV written to the
   SD card — and every WAV sent to ASR — had `byteRate=0`, `blockAlign=0`.
   `WavHeader.FieldValues` now pins this.
2. `SseTranscriptParser::handleLine()` bailed on the sticky `lineOverflowed`
   flag, so one over-long line permanently silenced the parser for the rest of
   the stream. The flag is now purely a report; the per-line `lineTruncated`
   state decides what gets parsed.

**Verification status**

| Gate | State |
| --- | --- |
| `pio run -e onepage` | PASS (Flash 86.2%, 5648188 B) |
| `test/stt_codec` (25 tests) | PASS (before and after the device fixes) |
| Device: record → WAV header bytes | PASS `hdr={byteRate=32000 blockAlign=2 bits=16}` |
| Device: no WiFi / no key paths | PASS `no WiFi, cannot transcribe` / `NO_KEY` |
| Device: clock | PASS `system clock unset, syncing via SNTP` → `clock set: epoch 1790325595` (≈1.1 s) |
| Device: TLS | PASS — no `E (...)` lines during the handshake |
| Device: record → POST → transcript | **PASS** `HTTP 200, request 205045 bytes` / `response 6095 bytes, 18 events, text 62 bytes, done=1 complete=1 lineOverflow=0` / `transcript (62 bytes): Hello, what's your name? Hello, hi,妹妹. What are you doing?` |
| End-to-end latency | 3.30 s (478503 → 481804 ms since boot) |
| Heap leak | none — steady Free 9643 B after the call vs 9447 B on the previous run |

**Known cost / follow-ups**

- The transcription runs on the activity loop: `New max loop duration: 3334 ms`
  (UI shows `STR_STT_TRANSCRIBING` and is frozen for that long). Next step is a
  worker task if that is unacceptable.
- `finishRecording()` writes the 153 KB WAV to SD synchronously (~1.3 s block).
- Internal heap is still tight with WiFi up (Free ~9.6 KB, MaxAlloc ~4.8 KB,
  Min Free ~2.4 KB) — that is P2, unchanged.

---

## P3-S · 语音对话 → 屏显 LLM 回复（方案 C）

## 7. 本轮已完成（2026-09-26）

| 项 | 结论 | 出处 |
|---|---|---|
| 网关 token 不匹配 | 设备 NVS 共享了陈旧 token；网页填入网关 token 并保存入 NVS 后，`stage=ok`、设备 `authenticated as <64-hex deviceId>` | `POST /api/cloud` 日志 `token=set`；探针 `{"stage":"ok","detail":"authenticated as ..."}` |
| 凭据脱敏 | 网页/日志/面板一律不回显明文 token；新增 `lib/OpenClaw/OpenClawRedact.h`（`payloadCarriesCredential`/`safeEcho`/`[redacted: credential]`），在 `OpenClawActivity` 6 处生效 | `OpenClawActivity.cpp`：CONNECTED 日志+statusLine、TEXT 帧转储、!settled statusLine、DISCONNECTED、ERROR、errorMessage、reply.error() |
| 单测 | 新增 `test/openclaw_redact/` + `test/openclaw_chat/` 剥标记用例 | **161/161** |
| 服务端失败标记 | 根因是 `agent:main:default` 会话历史被污染：同一 prompt 在 `default` 会话产生 **9 处**标记，在新建的 `xpt-fresh-1` 会话产生 **0 处** | 对照实验（本会话） |
| 会话隔离 | 设备改用 `sessionKey: "crosspoint"`；网关新建 `agent:main:crosspoint`（18856 input tokens / 25000 total）；旧的 `default` 仍 126K 令牌被污染 | `crosspoint_session.log`；网关 `openclaw sessions` |
| 文本往返验证 | `chat.send id 1, 157 bytes` → `chat delta frame 273 bytes, reply 4 bytes` → `[OCW] chat final: 4 bytes` → `[OCW] reply: PONG`，**无 `stripped ... failure markers` 行** | 设备串口日志 |

## 8. UI 设计与功能流程

### 8.1 设计约束

- **无喇叭、无 TTS**。回复只在屏上显示。AIWatch 的 `TTS_PLAYING`/`auto_read_response`/语音回环整段不移植。
- **e-ink 刷新纪律**：每态 1 次刷新；录音中 0 次刷新（mic 电源轨与 `EPD_RST` 共用 GPIO27，`HalGPIO.cpp:282,209`）；**delta 不绘制**（只画 final，与现有 `paintedPartial` 策略一致，`OpenClawActivity.h:130-133`）。
- **SCOPE 合规**：用户主动触发的单次往返（opt-in），不常驻 WiFi 任务，不影响阅读。`OpenClawActivity` 现有的 socket 生命周期（`onEnter` 打开、`onExit` 关闭）就是这个模型，`OpenClawActivity.h:33-39`。
- **无长按事件**：`MappedInputManager` 只有 `isPressed`/`getHeldTime`/`wasReleased`（`MappedInputManager.h:22-26`）。hold-to-talk 要自己轮询，收益不大，采用**短按**。
- **录音触发**（与 AIWatch 同款）：按一次开始 → 静音 1.5 s 或满 8 s 自动停 → Back 取消。`voice_chat.c:257-261,163`。
- **回答长度**：提示词要求首行标签 + 1~2 句（回复区有限，`OpenClawActivity.cpp:689` 现行 5 行截断）。

### 8.2 架构（三个共用层，一个新活动）

```
lib/voice/Recorder                    ← 从 MicrophoneTestActivity 抽出
                                        分块读(100ms/chunk)、VAD/静音停、DC 偏移、峰值/RMS
lib/OpenClaw/OpenClawSession          ← 从 OpenClawActivity 抽出
                                        ws + handshake + chat.send + 回复积累 + 重试 + 剥标记
src/activities/voice/VoiceActivity     ← 新增（Settings → Voice）
src/activities/settings/{MicrophoneTest,OpenClaw}Activity  ← 各自退化为纯诊断薄壳
```

- 两个诊断屏**完整保留**（mic 硬件路径是本项目最脆弱的，`docs §2.2`）。
- `VoiceActivity` 退出时：`onExit()` 关闭 socket + 释放全部 PSRAM buffer，**不影响阅读的电子书大内存**（同现有模式）。
- 全固件只有 `ActivityManager.cpp:23` 一个 `xTaskCreate`；v1 不引入新任务，录音与 STT 阻塞在活动 loop（先画"识别中"再转写 3.3 s）。worker task 列为后续项。

### 8.3 UI 布局（800×480，全部走 `GUI` 宏 + `getOrientedViewableTRBL`，四方向自适应）

```
┌──────────────────────────────────────────────────────────┐
│ ▶ Voice                   [history ▸]    [auto]  [send] │ ← Header：模式芯片、左侧历史入口
├──────────────────────────────────────────────────────────┤
│                                                          │
│  ┌──────────────┐                                       │
│  │ 2026-09-26   │   ← 主对话区（气泡列表）             │
│  │  我：…       │      新消息在底部，向上滚动           │
│  │  答：…       │      气泡区分 user / assistant       │
│  │  我：…       │      底部保留当前输入气泡            │
│  │  答：…       │                                       │
│  │  ···         │                                       │
│  └──────────────┘                                       │
│  ┃ 竖式滚动条 ┃                                         │ ← 向上拉动翻旧消息
│                                                          │
├──────────────────────────────────────────────────────────┤
│ 输入区：<当前转写气泡>           [发送]     [取消]       │ ← 手动模式；自动模式仅显示 auto 标记
└──────────────────────────────────────────────────────────┘
```

- **历史面板（左侧）**：按日汇总，每个日期下是预览条目（时间 + 首行文字）。tap → 该对话填充主区。列表可收起。
- **气泡**：user / assistant 分别配色；超出一屏时以**竖式滚动条**上拉查看（旧消息在上方）。
- **模式芯片**：`auto` / `manual`。
- **历史落盘**：`.crosspoint/chat/` 下按日 JSONL（`{role, text, ts}`），复用既有 SD 缓存根（`.crosspoint/`）。不落 NVS（避免整文件重写的写放大）。
- **输入区（manual）**：转写文本显示为当前气泡，按 `Confirm` 才发 `chat.send`。
- **输入区（auto）**：转写文本出现即自动发 `chat.send`，仅显示 `auto` 标记。

### 8.4 状态机（每态 1 次刷新）

```
Idle ──Confirm──> Listening ──静音1.5s / 8s──> Transcribing ──STT ok──> Sending ──final──> Answer
  ▲                                                │               │
  │  Back                                           │  空/失败      │  Back / 90s超时
  └─────────────────────────────────────────────────┘→ Idle（提示原因）
```

- **Listening**：静态"正在聆听…"，录音中**不刷新**。
- **Transcribing**：显示"识别中…"，阻塞 ~3.3 s。
- **Sending**：显示"发送中…"，`chat.send` 已发出，WS 保持打开等待回复。
- **Answer**：只画 final 气泡；转写文本先作为 user 气泡落历史，LLM 回复作为 assistant 气泡落历史。

### 8.5 阶段计划

| 阶段 | 内容 | 验证 |
|---|---|---|
| **0 · 并存实验** | 在 `Connected` 态按 Left 触发 `STT::transcribe`（2 s 静音缓冲），记录内部堆前后值，并确认 WS 未断连 | **已执行，PASS（2026-09-26）**：`heap probe: OK key=1 http=200 req=85577 ws=alive`；WiFi 下内部堆 Free≈4 KB / MaxAlloc≈3.8 KB / MinFree≈267 B，WS 不断。坑：`ESP.getFreePsram()` 在本 build 恒为 0（`psramFound()` 门控，SPIRAM 堆实际活着，`src/main.cpp` MEM 行有同样记载），探针已改用 `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`；OpenClawActivity 面板诊断行同修 |
| **1 · 语音屏** | 抽 `Recorder` + `OpenClawSession`；新建 `VoiceActivity`；气泡渲染；历史面板 + SD 持久化；auto/manual 模式；退出清理 | **代码+设备已验证 PASS（2026-09-27，见 §10.5）**：两轮完整对话（录音→STT→chat.send→final→屏显气泡）；历史面板/SD 持久化/auto-manual 模式尚未实现（见 §9） |
| **2 · 阅读快捷唤起** | 阅读状态下按键唤起 `VoiceActivity`（返回时恢复阅读进度），复用 Phase 1 的 session 层 | 边读边问，返回后从断点继续翻页 |
| **3 · 打磨（延后）** | 多轮会话历史在 LLM 端的延续、长回复分页、提示词调优、录音期间真实麦克风路径 | 人工评测 |

### 8.6 设计期望（待后续评估）：chat.deepseek.com 风格对话界面

用户期望的 UI 形态（Phase 1 实施时再择一落地，当前 §8.3 为工作草案）：

- **整体形态**：类似 chat.deepseek.com 的对话页——顶部为消息气泡列表，新消息在底部，更早的聊天**向上滚动**查看；需要一个**向上拉的动作条/滚动条**才能看到更早的对话。
- **对话框**：消息以气泡形式区分 user（自己）与 assistant（LLM），气泡内显示文本。
- **语音模式选项按钮**：`auto` / `manual` 两个选项。
  - **auto**：说话 → STT 文本直接出现在对话框内 → **自动**发送给 OpenClaw 服务器。
  - **manual**：说话 → STT 文本出现在对话框内但**不自动发送**，需手动按下发送按钮才发 `chat.send`。
- **历史记录**：位于**左侧**，按**日汇总**的预览列表（日期 + 条目摘要）；点开某一日期/条目可查看该次历史聊天的完整内容。
- **历史落盘**：内容保存在 **SD 卡文件夹**下（`.crosspoint/chat/` 按日 JSONL），不落 NVS。
- **退出清理**：退出 OpenClaw 时关闭相关资源（socket、PSRAM buffer），**避免影响阅读时占据大内存的电子书功能**。
- **评估要点**：左侧历史面板在 800×480 e-ink 上占位比例、气泡列表在长对话下的滚动与内存开销、按日 JSONL 的读写与碎片、与现有 EPUB 缓存目录 `.crosspoint/` 的共存策略。

## 9. 待办（本次会话）

1. ~~执行阶段 0 并存实验~~ → **已完成，PASS**（见 §8.5 阶段 0 行与 §10）。
2. ~~Phase 1 编码 + 设备验证~~ → **已完成，端到端 PASS**（两轮完整对话，见 §10.5）。
3. **收尾秘密清理**：`/tmp/opencode/gw_token.txt`、`/tmp/opencode/ocw/askpass`。
4. ~~WS 断线韧性~~ → **已实现**：RFC6455 心跳保活（25 s ping）+ poll() 内自动重连（退避 15/30/45 s，上限 3 次），见 §10.6；断线触发路径待人工验证（拔 WiFi/网关重启）。
5. **可选优化**：I2S DMA 环从 1024 B 调到 2048 B（2×256），把丢样率从 4% 降下来（当前 MaxAlloc≈3.8 KB 下 1024 B 可靠，2048 B 需实测 WiFi 下的 MaxAlloc 余量）。
6. ~~**Phase 1 收尾功能**（§8.3/§8.6）：历史面板 + SD 持久化（`.crosspoint/chat/` 按日 JSONL）、auto/manual 模式~~ → **已实现（2026-09-27）**，见 §10.7；设备端验证（历史落盘→重进→回看、四方向渲染）待人工执行。
7. **Phase 2**：阅读状态快捷唤起 VoiceActivity（§8.5 阶段 2）。
8. ~~首页 Voice 入口 / 设置项精简 / 历史键位与提示 / 回复不截断 / 图标方向 / 中文 i18n / 录音先画后开麦~~ → **已完成并烧录，设备串口 0 错误**；目检（图标朝向、中文菜单、侧边提示位置）待人工确认，见 §11.1/§11.2。
9. **“卡在等待回复”** → 代码层修复 + 串口复现工具已就绪，**待烧录验证**；网关在工具轮里发什么帧（决定是否要“迟到 final 仍接收”）尚未抓到，见 §11.3。

## 10. 本轮进展（2026-09-27）

### 10.1 Phase 0 结果（设备实测 PASS）

- `OpenClawActivity` Connected 态按 Left：2 s 静音 PCM → STT 上传成功（`key=1 http=200 req=85577`），WS 存活。
- WiFi 开启后内部堆真实水位：Free≈4 KB / MaxAlloc≈3.8 KB / MinFree≈267 B。**最大分配块 2548 B > 0**，结论：小缓冲可用，大缓冲（>MaxAlloc）必须走 PSRAM。
- PSRAM 实测：`heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` ≈ 8.3 MB（计划文档早先写 2 MB 已过时）。`ESP.getFreePsram()` 在本 build 恒为 0，一切 PSRAM 水位统计必须用 `heap_caps_*`。

### 10.2 I2S DMA 与 WiFi 共存修复（设备实测 PASS）

- **根因**：I2S STD RX 的 DMA 描述符必须落在**连续的 DMA-capable 内部 RAM**。原配置 4×256 帧 = 4096 B > WiFi 下的 MaxAlloc≈3.8 KB → `i2s_channel_init` 返回 `init_std_rx failed: 257`（ESP_ERR_NO_MEM）。
- **修复**（`lib/hal/HalMicrophone.cpp`）：`dma_desc_num` 4→2、`dma_frame_num` 256→128（DMA 环 4096 B→**1024 B**）、`RAW_WORDS_PER_READ` 512→256（单次读上限=半环，见该文件注释）。
- **实测**：WiFi 开启时 `[MIC] STD RX up ... dma buf 1024 B`，5 s 录音+转写全通；丢样 4%（76800/80000 samples），见 §9 待办 5 的可选优化。

### 10.3 Phase 1 代码（编译通过，待设备验证）

| 层 | 文件 | 要点 |
|---|---|---|
| 采集 | `lib/voice/Recorder.{h,cpp}`（Phase 1-1） | `begin(sampleRate, maxSeconds, silenceMs=0)` / `poll()` / `end()` / `close()`；VAD=音频时间计（100 ms chunk），`setVadThreshold(rms)`、`voicedMs()`、`endedBySilence()`、`VOICED_MIN_MS=400`；MicrophoneTestActivity 已改用且行为不变 |
| 会话 | `lib/OpenClaw/OpenClawSession.{h,cpp}`（Phase 1-2） | `Session`：State{Idle/Connecting/Handshake/Challenge/Connected/Failed}、`sendMessage()`→SendOutcome、retry/90 s 超时在 `poll()` 内处理、`setNotifier` 用函数指针（避免 `std::function` 堆分配）；握手比较用命名空间级 `OpenClaw::Phase::Failed` |
| UI | `src/activities/voice/VoiceActivity.{h,cpp}`（Phase 1-3） | 六态 Idle/Listening/Transcribing/Sending/Answer/Failed；MAX_RECORD_SECONDS=8、SILENCE_STOP_MS=1500、VAD_RMS_THRESHOLD=1200；每态 1 次刷新，录音中 0 刷新（mic 电源轨与 EPD_RST 共用 GPIO27）；文案全部 `tr()`（STR_VOICE_* 六键） |
| 接线 | `src/activities/settings/SettingsActivity.*` | `SettingAction::Voice` + 菜单项 + launch case（include 走 `../voice/VoiceActivity.h`） |

- 主机单测：SttCodecTest 25/25、OpenClawChatTest 17/17 PASS（本轮协议代码未动）。
- 构建：`pio run -e onepage` SUCCESS（DIRAM 233156 B / 90.86%，Flash 88.3%，与上轮持平）。
- 已知观察项：Sending 态在握手未完成时会等到 Connected 再发（防丢用户录音）。

### 10.4 构建环境事故与修复（host 工具链，非固件问题）

- 现象：`pio run` 报 `ModuleNotFoundError: SCons.Tool.FortranCommon`，随后 `Failed to install Python dependencies (exit code: 2)`。
- 根因链：penv 里 `platformio` 被升级到 6.2.0（官方版），而 `platformio.ini` 的 pioarduino 平台 55.03.37 自带 `builder/penv_setup.py`，它把 penv 的 `platformio` **钉在自家 fork v6.1.19**（包名 `pioarduino-core`），还钉了一组依赖（`urllib3<2`、`zopfli` 等）。版本不符 → 每次构建都触发 uv 重装 → 组合解析失败且 stderr 被吞；PIO 自动升级同时把 `~/.platformio/packages/tool-scons` 删了。
- 修复：penv 恢复到钉版状态——`uv pip install "platformio==6.1.19" "urllib3<2" "zopfli>=0.2.2"` + `pio pkg install -g -t "platformio/tool-scons@~4.41101.0"`。**不要升级 penv 里的 platformio**（PIO 的自动升级提示 `pio upgrade` 同样危险），否则下次构建又会循环重装。

### 10.5 设备验证（2026-09-27，Phase 1-3 端到端 PASS）

两轮完整对话，串口证据（`/tmp/serial.log`）：

**第 1 轮**（WiFi 已连、会话已就绪的状态下）：

```
[VOICE] transcript (33 bytes): 英英英语，厨房怎么说？
[OCS] chat.send id 4, 160 bytes
[OCS] chat delta: frame 280 bytes, reply 7 bytes
[OCS] chat final: 7 bytes
[OCS] reply: Kitchen
[OCS] chat round trip complete
```

**第 2 轮**（重进 Voice 后从零验证采集→STT→发送全链路）：

```
[MIC] STD RX up: 16000 Hz PCM, decim 64, bclk 1024000 Hz, dma buf 1024 B   ← WiFi 下 DMA 共存配置生效
[REC] recording 8 s (128000 samples @ 16000 Hz, vad on)                    ← VAD 静音检测开启
[STT] POST 246444 WAV bytes -> 328777 body bytes                           ← 流式上传，非一次性
[STT] HTTP 200, request 328777 bytes
[STT] response 3808 bytes, 10 events, text 39 bytes, done=1 complete=1
[VOICE] transcript (39 bytes): 你好，小智，你叫什么名字？
[OCS] chat.send id 1, 166 bytes → final: 109 bytes
[OCS] reply: 我还没有名字呢，IDENTITY.md 里还是空的。你想给我起一个吗？"小智"就挺好听的 😄
[OCS] chat round trip complete
```

要点：

- **流式上传在真实 8 s 录音上工作**：246 KB WAV → 328 KB JSON 体全部走 `WavBase64Writer` 分块，`HTTP 200`、服务端完整解析（`lineOverflow=0`）。
- **录音期间 e-ink 零刷新**（GPIO27 电源轨约束）成立：Listening→Transcribing 之间无 GFX 刷新日志。
- **CJK 字形按需加载**（`[SDCF] Overflow: loaded U+667A ... slot 113/128`）与 WS 流量并存无异常。
- **观察项**：会话空闲 ~55 s 后 WS 被断开（`[OCS] WS disconnected: ... failed: Connection lost`），VoiceActivity 转 Failed 态并提示；重进 Voice 即自动重连重握手，再次完整对话成功 → 已列入 §9 待办 4（Session 层自动重连/保活）。
- 验证后继续 host 侧收尾：psram 面板 bug 修复 + 本节文档更新，`pio run -e onepage` SUCCESS。

### 10.6 WS 保活与自动重连（2026-09-27，基于 §10.5 的观察项）

**保活**（`Session::startConnect()`，`lib/OpenClaw/OpenClawSession.cpp`）：`ws_->enableHeartbeat(PING_INTERVAL_MS=25000, PONG_TIMEOUT_MS=5000, PONG_TIMEOUT_COUNT=3)`，走链接2004库自带的 RFC 6455 ping/pong（`handleHBPing`/`handleHBTimeout`，pong 超时 3 次主动断开→触发重连路径）。目的：网关实测空闲 ~55 s 断开；25 s 间隔把链路钉住，且 pong 静默能主动发现半死 TCP（否则下一个 chat.send 才发现，用户录音白录）。

**自动重连**（`poll()` 驱动，事件回调只置位）：

- 断线回调（`WStype_DISCONNECTED/ERROR`）→ `scheduleReconnect()`：退避 `RECONNECT_MS×(attempt+1)`（15/30/45 s），预算 `MAX_RECONNECTS=3`，超出→ `fail(STR_OPENCLAW_DISCONNECTED)`，用户手动重试。
- 等待到期 → `redial()`：复用同一 `WebSocketsClient` 重拨（库自身 loop() 也支持重拨，但状态机由 Session 驱动；begin() 重建连接字段，重挂 heartbeat），省去一次性分配（chat buf/CA/handshake 对象不重建）。
- TCP 重连成功（`WStype_CONNECTED`）→ `handshake_->configure()` 重置握手状态机（握手是有状态的，challenge 重走一遍；复用 CLOUD_CFG 中已刷新的 device token）。
- 认证成功 → `reconnecting_=false`、`reconnectAttempts_=0`（预算复原）。
- **认证被拒仍致命**（`Phase::Failed` → `fail()`）：网关拒绝不是天气问题，重试无意义；approveCommand 路径不变。
- 回复中断线的处理：`awaitingReply_` 清为 `SendOutcome::Failed`，重连后不自动重发（网关幂等键去重，重发有双回复风险）；VoiceActivity 收到断线通知→回 Idle，保留转写文本，Confirm 重录。

**设备观察**（烧录后连续 ~25 min）：`[OCS]` 无 WS disconnected/reconnect 日志，会话保持活跃（含多次 chat 往返与 `[OCS] WS event 9/10` ping/pong 帧）→ 保活生效，网关 55 s 空闲断开不再出现。断线重连路径（拔 WiFi/网关重启）待后续人工触发验证。

### 10.7 Phase 1 收尾（2026-09-27，代码完成 + 主机测试通过，设备验证待做）

§9 待办 6 落地：历史 SD 持久化 + 历史面板 + auto/manual 模式。

| 层 | 文件 | 要点 |
|---|---|---|
| 格式 | `lib/OpenClaw/ChatHistoryFormat.h`（纯 C++，可主机测试） | `dayPath()`（civil_from_days，本地时区偏移取日，无 SNTP 时拒绝落盘）；`buildLine()`：`OCL1 u|a <tsMs> "text"\n`，逐 UTF-8 序列转义（截断不撕裂 CJK/转义序列），控制字符 → `\u00xx`（正文换行不会破坏单行记录）；`parseLine()`：严格 OCL1 形状 + JSON 字符串反转义（含代理对）|
| 存储 | `lib/OpenClaw/ChatHistory.{h,cpp}` | `append()`：HalStorage `O_WRITE\|O_APPEND\|O_CREAT`，一次往返一行，目录 `/\.crosspoint/chat/`；`listDays()`：目录名排序（YYYY-MM-DD 字典序 = 时间序），最新在前，最多 14 天；`loadDay()`：逐行流式读入调用方 PSRAM 池（文本从池尾向前打包，池满丢最旧 → 永远保留最新一段），无 String、无整文件读 |
| UI | `src/activities/voice/VoiceActivity.*` | 新增 `Review`（manual 态：转写就绪，Confirm 才发）、`HistoryDays`（日列表，Up/Down 选、Confirm 开、Back 退）、`HistoryDay`（单日记录，Up/Down 翻、显示角色 + 位置 n/N + 5 行包裹文本）；Right 在 Idle/Answer/Failed/Review 切换 auto/manual 并 `SETTINGS.saveToFile()`；往返完成（Answer 态）时 `recordHistoryTurn()` 追加 user+reply 两行 |
| 设置 | `CrossPointSettings.h` + `JsonSettingsIO.cpp` | `voiceAutoSend`（0=manual 默认 / 1=auto），JSON 键 `voiceAutoSend`，clamp 到 0/1；放在 `ONEPAGE_C61` 守卫之外（VoiceActivity 全环境编译）|
| 文案 | `english.yaml` | 新增 `STR_VOICE_MODE_AUTO/MANUAL/MODE_TO/REVIEW_HINT/SEND/HISTORY/HISTORY_EMPTY/NO_HISTORY_DAY/HISTORY_SAVED`（其余语言回落英文）|
| 测试 | `test/chat_history/` | 14 个主机用例：日期边界（UTC±偏移跨日、无 SNTP 拒绝）、ASCII/UTF-8/引号/控制字符往返、截断保 UTF-8 完整性、垃圾行拒绝、代理对解码 |

- 主机测试：**181/181 PASS**（新增 14）；构建：`pio run -e onepage` 与 `-e default` 均 SUCCESS（onepage RAM 37.8%，Flash 88.4%）；`pio check` 无新增告警（ChatHistoryFormat 两条 low-style 提示为有意为之，见代码注释）。
- 内存：历史 PSRAM 池 6144 B，`onEnter()` 分配（PSRAM 优先、内部回退）、`onExit()` 释放，与 §8.2 退出清理承诺一致；不新增任务。
- **待设备验证**：完成一轮对话 → 串口 `[OCH] history +1 line` → 重进 Voice → Left 看到当日 → 打开看到两行记录；manual 模式下转写后停在 Review、Confirm 后才发；Right 切换后重启保持；四方向渲染不越界。
- 已知边界：`recordHistoryTurn()` 用回答落定时刻的时间戳，user/reply 两行同 ts；截断的历史正文（>160 B 解码缓冲的行）按行跳过并在 DBG 日志注明——当前 256 B 的 `buildLine` 上限内不会发生。

## 11. 本轮进展（2026-09-27 晚）：对话屏收口 + “等待回复”卡死排查

### 11.1 入口与渲染收口（已烧录，设备串口 0 错误）

| 项 | 内容 | 出处 |
|---|---|---|
| 首页 Voice 入口 | `HomeMenuItem::VOICE` + `goToVoice()`，首页菜单变 5 项；`goHome("Voice")` 把返回首页的落点指到该项 | `src/activities/home/HomeActivity.{h,cpp}`、`src/activities/ActivityManager.cpp:228` |
| 设置精简 | 删掉 `SettingAction::MicrophoneTest`/`OpenClaw` 的枚举值、菜单项与分发 case；`MicrophoneTestActivity.*`、`OpenClawActivity.*` 整文件删除（约 1400 行）。Flash 88.5% → **88.3%** | `src/activities/settings/SettingsActivity.*` |
| 历史键位与提示同源 | `prevKey`/`nextKey` 统一由 `mappedInput.isNavDirectionSwapped()` 推导：历史开在 `nextKey`、模式切在 `prevKey`，提示框也按同一映射摆放（原先“Left 开历史、提示却画在 Right 上”） | `VoiceActivity.cpp` `loop()`、`mapLabels()` |
| 回复与历史不截断 | `wrappedText` 支持硬换行、`fitPrefixLen` 用 UTF-8 二分避免撕字、省略号只加在末行；历史池 32768 B（回退 6144 B），`drawTextWindow()` 窗口滚动 + `n-m/total` 指示 | `lib/GfxRenderer/GfxRenderer.cpp`、`lib/OpenClaw/ChatHistory.*` |
| 进屏自动联网 | `onEnter()` 里 `WiFi.status() != WL_CONNECTED` 就先开 `WifiSelectionActivity`（autoConnect 直连上次网络），成功后再 `startSession()` | `VoiceActivity.cpp` `onEnter()` |

验证：`pio run -e onepage` SUCCESS、`pio run -t unit-tests` 181/181、烧录后连续抓取 12 s 无 `[ERR]`/panic。

### 11.2 图标 / i18n / 布局 / 状态可见（已烧录，待目检）

| 问题 | 根因与修法 |
|---|---|
| 菜单里的麦克风图标躺倒 90° | `GfxRenderer::drawIcon()` 的约定是**屏幕图像 = 存储位图顺时针转 90°**（用 `wifi.h`/`folder.h` 解码验证：只有 CW 旋转才是正立的 wifi 弧线和带折角的文件夹）。`src/components/icons/voice.h` 原本存的是正立图 → 改存**逆时针转 90°**（生成脚本重跑 + round-trip 自检） |
| 语音相关界面在中文系统里仍显示英文 | `STR_VOICE_*`/`STR_MIC_*`/`STR_STT_*`/`STR_OPENCLAW_*` 共 49 个键只存在于 `english.yaml`，其余语言全缺 → 回落英文。已补齐 `chinese_simplified.yaml`/`chinese_traditional.yaml`（各 468/468，`gen_i18n` 报 missing=0），另新增 5 个键（阶段行“录音/转写/发送/回复”与录音提示）。注意 i18n 解析器**不认注释行**，yaml 里不能写 `# …` |
| 历史页侧边上下提示偏下 | `LyraTheme.cpp` `topHintButtonY` 345→**305**、`BaseTheme.cpp` `topButtonY` 345→**305**（两个 78 px 提示框整体上移 40 px，块中心 425→≈385，物理按键旁对齐）。RoundedRaff 继承 Base，三套主题一致；X3 路径 `x3ButtonY` 未动 |
| 按下对话键后录了才知道有没有画界面 | `startRecording()` 改为**先画后开麦**：`state_=Listening → requestUpdateAndWait()` 等刷新完成 → `MIC.begin()`。录音期间 0 刷新（mic 电源轨与 `EPD_RST` 共用 GPIO27，且刷新大电流会污染 PDM 采样） |
| “发出去之后到底在干嘛”看不见 | 新增 `drawPipeline()`（阶段行 `[录音] > 转写 > 发送 > 回复`，当前阶段加粗方括号）与 `gatewayStatus()`（连接中/握手/等待认证/已连接/已断开/失败原因）；录音、转写、等待三态都画图标 + 网关状态行 |

### 11.3 “卡在等待回复” —— 排查与修复

**症状**：让设备定个闹钟，界面停在“等待回复…”不动。

**日志/复现**：新增两条串口命令用于不依赖语音的复现——`CMD:GO_VOICE`（直接进语音屏）、`CMD:VOICE_TX <文本>`（顶替 STT 步骤发一轮，走 `VoiceActivity::injectTranscript()` 静态钩子，`ActivityManager::injectVoiceTranscript()` 转发）。22:30:02 发 `GO_VOICE`、22:30:27 发 `VOICE_TX 帮我定一个明早8点的闹钟`，随后 **`/dev/ttyACM0` 从 USB 总线消失**（`lsusb` 已无设备），没能抓到 `[OCS] WS text` 帧；崩溃/重启与物理拔线尚无法区分。设备回来后：先看 `/crash_report.txt` 和启动日志，再跑这两条命令抓网关帧。

**代码层已确认的三个缺陷**（不需要服务端日志即可证明）：

1. **第二轮可能永远不发** —— `VoiceActivity.cpp` 的 Sending 分支原来用 `session_.outcome()==Sent` 判断“已经发过了”。但 `outcome_` 描述的是**上一轮**：本轮若在会话尚未 `Connected` 时进入（`sendToOpenClaw()` 原分支只置状态就 return），等重连完成后 `outcome()` 仍是上一轮的 `Sent` → 既不发新帧，又因为 `reply_`（上一轮的回复，只在 `sendMessage()`→`attach()` 时才 reset）非空而 `replySize()>0 && outcome==Sent` 成立 → **直接把上一轮的回答当成本轮答案刷出来**，或在更坏的时序下停在发送屏。→ 引入 `oweSend_`（“还欠一次 chat.send”），`sendPending()` 在 `state()==Connected` 时补发，发送前不进结算分支。
2. **超时按总时长算，工具轮会被误杀** —— `Session::poll()` 用 `millis()-sentAtMs_ >= 90s`。本文件早已记录普通一轮实测 41 s、其中网关前 39.7 s 完全静默（§10.5 观察项 / `OpenClawSession.h` 旧注释），而“定闹钟”会走工具调用，总时长更长；更糟的是超时后 `if (awaitingReply_) handleChatFrame()` 会把**迟到的 final 直接丢掉**。→ 每个 `chat delta` 到达即 `sentAtMs_ = millis()`（改成**静默超时**），`CHAT_TIMEOUT_MS` 90 s → **120 s**。
3. **等待期零反馈** —— 等待屏只画一次，之后 90~180 s（失败标记重发还有一轮）画面纹丝不动，观感就是死机。→ `Session::awaitingMs()` + 标题显示 `等待回复… 15/120s`，`WAIT_TICK_MS = 15000` 触发一次重绘（e-ink 代价上界 8 次/轮，delta 仍不逐帧重绘，符合 §8.1 的刷新纪律）。

### 11.4 验证状态

| Gate | State |
|---|---|
| `pio run -e onepage` | PASS（RAM 37.8% = 123972/327680 B，Flash 88.3% = 5789906/6553600 B） |
| `pio run -t unit-tests` | PASS **190/190**（+9 个 markdown 剥壳用例，2026-09-28） |
| clang-format（改动文件） | PASS |
| 烧录 §11.1/§11.2 | PASS（串口 12 s 0 错误） |
| 烧录 §11.3 + 复现定闹钟 | **PASS（2026-09-28）：工具轮 10.6 s 全链路成功**，`CMD:VOICE_TX 帮我定一个明早8点的闹钟` → thinking 流 → `tool: automations` → cron added → final `搞定!⏰ 明早 8:00 会叫你起床。`（日志 `/tmp/voice_tx3.log`） |
| M0-2/M0-3 设备复验 | PASS（2026-09-28，`/tmp/verify_m0.log`）：回复无裸 markdown 符号；9 个 tick/health 帧 0 个引发空闲重绘；期间网关两次断链均自动重连成功 |

### 11.5 未决 / 下一步

1. ~~设备回插 → 查 `/crash_report.txt` → 烧录 §11.3 → 重跑复现~~ → **已完成（2026-09-28）**：无 panic dump；工具轮 10.6 s 全链路 PASS，§11.3 三项修复在设备上成立。
2. ~~迟到 final 仍接收~~ → **撤销，无需实现**：网关在工具轮持续发 thinking/status/tool 帧（实测最长静默 ~4 s），120 s 静默超时不会误杀。
3. ~~网关事件帧每帧触发 e-ink 重绘~~ → **已修复（M0-2，2026-09-28）**：`OpenClawSession` Connected 态不再无条件 `paint`，`handleChatFrame` 仅在首个可见 delta/final/error 通知；设备复验 9 个 tick/health 帧 0 个重绘。
4. ~~回复 markdown 符号裸上屏~~ → **已修复（M0-3，2026-09-28）**：`ChatReply::stripMarkdownMarkers()`（`**`/`*`/反引号，行界约束，`_` 不动），在 Final 定稿处调用，面板/历史/日志统一生效；9 个主机用例。
5. USB 掉线两次均发生在 WiFi 上电/烧写瞬间，5 s 内自动重枚举、无 crash report → 判定硬件侧（供电/线缆）；换线复测仍待人工执行。
6. §9 待办 5（I2S DMA 环 1024→2048 B，丢样 4%→更低）未动。
7. 目检：图标朝向、中文菜单、侧边提示位置、录音先画后开麦。
8. 后续开发按 [v4.0-development-plan.md](v4.0-development-plan.md) 推进（M1 可靠性起步）。

