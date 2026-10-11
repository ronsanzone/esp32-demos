# Handoff — ESP32-C6 AMOLED demos (devdeck)

_Written 2026-10-11 at the end of the first working session. Records the state as of that date._

![devdeck screens](docs/screenshots/overview.png)

## TL;DR

- **Board:** Waveshare **ESP32-C6-Touch-AMOLED-2.16**, connected over USB-C at `/dev/cu.usbmodem101`.
- **Toolchain:** ESP-IDF **v6.1** is installed at `~/esp/esp-idf`. Load it with the `get_idf` zsh alias.
- **`devdeck` firmware:** four swipeable screens fed live from the laptop over USB. It's flashed on the board now, built from this repo.
  1. **Agent Orbit**: each live Claude Code session is a planet, and every tool call is a colored spark into the core.
  2. **Context**: a gauge of each live session's context-window fill (tap to cycle), with cache-hit % and a per-turn sparkline.
  3. **Deep Work**: your deep-work pipeline (Ticket → Questions → Research → Design → Outline → Plan → Build → Ship), with each node sized by how often its skills ran.
  4. **Ship Pulse**: a 26-week GitHub contribution heatmap, open PRs with check status, and 14-day bars.
- **Data source:** `tools/deck.py bridge` on the laptop. It tails `~/.claude/projects/**/*.jsonl` and calls `gh`.
- **Hands-off mode:** untouched for 60 s, the screens auto-rotate every 15 s.
- **No Wi-Fi.** It was built, verified, then removed because it cost 167 KB of the 228 KB free heap (see [Decisions](#decisions)).
- **Not yet verified by a human:** panel colors and orientation on the glass, and touch (swipe, tap-to-cycle). Everything else was verified from on-device framebuffer snapshots (see [Verification](#verification-status)).

## Launcher and apps (added 2026-10-11)

One firmware holds six apps. The board runs **one app per boot**: picking another writes its id to NVS and reboots, so every app starts on a clean heap. That matters because the radio stacks and LVGL can't share 512 KB with each other and with devdeck's screens.

```
 home (launcher grid) ──tap card──► NVS deck/app = "lens", healthy = 1 ──► esp_restart
        ▲                                                                       │
        │  BOOT button (GPIO9), `deck.py app home`, or any reset             ▼
        └─ (crash, hang + power cycle) before the app has run 5 s        apps_boot() reads deck/app,
                                                                         sizes LVGL strips, runs app
```

| id | App | LVGL strip | What it uses | Notes |
|---|---|---|---|---|
| `home` | Launcher | 100 lines | touch | 2×3 grid of generative cards, free-heap readout |
| `devdeck` | The original four screens | 100 | USB bridge | unchanged behavior |
| `painting` | Living Painting | 100 | flash `storage` partition | Gray-Scott reaction-diffusion, 96×96 Q14 fixed point, upscaled to 480×480. Tap seeds a bloom, long-press cycles palettes (reef, ember, moss, ink). `commit` messages seed big blooms, Edit/Write tool calls small ones. Saves every 5 min into 32 rotating 40 KB slots (CRC-checked), so it keeps growing across reboots |
| `lens` | Sound Lens | 100 | ES7210 mics over I2S (TDM, 16 kHz) | 512-point fixed-point FFT, log-frequency waterfall 40 Hz–8 kHz with auto-ranging, McLeod-style pitch detection for the tuner, level meter |
| `field` | Signal Field | 40 | Wi-Fi promiscuous (ch 1–13 hop), NimBLE passive scan, 802.15.4 promiscuous (ch 11–26 hop) | Orbs: distance = RSSI, angle = address hash, size = chattiness, color = protocol. Tap an orb for details. Each radio is optional; whatever starts is drawn |
| `ghost` | Ghost Sense | 40 | Wi-Fi CSI from the strongest router | Ridgelines of the subcarrier amplitude profile. Motion = short-window wobble vs. a learned quiet floor. Breathing estimate (autocorrelation, 6–30/min) only after 30 s of stillness, marked experimental. Falls back to the busiest transmitter on that channel if the router's frames carry no CSI |

**Privacy:** Signal Field and Ghost Sense only listen. Addresses and network names stay in RAM and are never logged or sent to the laptop. Ghost Sense forgets fallback candidates as soon as it has picked one.

**Radio memory:** `sdkconfig.defaults` turns on BT (NimBLE, observer role only), CSI and coexistence, and shrinks the Wi-Fi buffers (6 static RX, 16 dynamic RX, 8 dynamic TX, AMPDU off, IRAM optimizations off). Static DIRAM is about 261 KB used, 191 KB left before the heap. The radio apps use 40-line strips (about 77 KB instead of 192 KB) to leave room for the stacks. **Heap with all three radios running is unmeasured**; the start-up log line `radios: wifi 1 ble 1 15.4 1, free heap N` is the number to check.

**How to switch apps:**
- Tap a card on the home screen.
- Press **BOOT** from any app to go home.
- `tools/deck.py app <id>` from the laptop. If the bridge is running it queues the switch through the bridge (it owns the port); otherwise it sends directly.
- Raw: `tools/deck.py send '{"t":"app","id":"field"}'`.

The board prints `@@APP <id>` once per boot, and the bridge logs `board app: <id>` when it sees it.

## Quickstart

```bash
get_idf
```
The alias defined in `~/.zshrc` puts Homebrew python3.14 ahead of macOS python 3.9, then sources `~/esp/esp-idf/export.sh`.

```bash
cd firmware/devdeck && idf.py build && idf.py -p /dev/cu.usbmodem101 flash
```
A first build on a clean checkout fetches the managed components (takes a few minutes). If this checkout was built before the launcher, `rm sdkconfig` (or `idf.py fullclean`) first so the radio settings in `sdkconfig.defaults` apply.

```bash
cd tools && /opt/homebrew/bin/python3.14 -m venv .venv && .venv/bin/pip install -r requirements.txt
```
One-time setup for the laptop tools.

```bash
tools/.venv/bin/python tools/deck.py bridge
```
Keep this running to feed live data (Ctrl-C stops it). **Stop it before flashing**: it holds the serial port.

```bash
tools/.venv/bin/python tools/deck.py shot 2
```
Saves a PNG of screen 2 to `tools/shot.png`. It only works while the bridge is running, because the bridge takes the snapshot.

Other commands:
- `tools/deck.py send '{"t":"ping"}'` sends one message and waits for the ack. Run it **without** the bridge running.
- `tools/deck.py snap out.png` takes a direct snapshot, also without the bridge.
- `python tools/serlog.py /dev/cu.usbmodem101 8` reads the boot log for 8 s and survives the USB re-enumeration that happens on reset.

## Hardware

Waveshare ESP32-C6-Touch-AMOLED-2.16 ([Amazon listing](https://www.amazon.com/dp/B0GWJ3L5BV)). Vendor repo: <https://github.com/waveshareteam/ESP32-C6-Touch-AMOLED-2.16>, cloned at `~/esp/ref/ws`.

| Part | Detail |
|---|---|
| SoC | ESP32-C6 (QFN40) rev v0.2: single RISC-V HP core at 160 MHz plus an LP core; Wi-Fi 6, BLE 5, 802.15.4. 512 KB SRAM, **no PSRAM**, 16 MB flash |
| USB | Native USB-Serial/JTAG (VID 0x303A). Appears as `/dev/cu.usbmodem101`; no driver needed |
| Display | 2.16" AMOLED, 480×480, **CO5300** controller over QSPI (driven by the `esp_lcd_sh8601` driver), 16-bit RGB565 |
| Touch | **CST9220** over I2C (driven by `waveshare/esp_lcd_touch_cst9217`; reports chip ID `0x9220`) |
| PMU | **AXP2101** at I2C `0x34`. Its ALDO3 rail powers the panel, and power-cycling ALDO3 *is* the panel reset |
| Also on board | QMI8658 6-axis IMU, PCF85063 RTC, dual mics plus audio codec (I2S), battery charging, KEY + BOOT buttons, SD card. **None of these are used yet** |

**Pin map, as actually used.** Waveshare's `user_config.h` lists different values. Its `DisplayPort` constructor defaults are what really run, and the touch chip ID confirmed them:

| Signal | GPIO |
|---|---|
| I2C SCL / SDA (PMU, touch, IMU, RTC) | 7 / 8 |
| LCD QSPI CLK, D0–D3, CS | 0, 1–4, **15** |
| Touch INT / RST | **5** / 11 |
| I2S MCLK / SCLK / LRCK / DOUT / DIN | 19 / 20 / 22 / 23 / 21 |
| SD CS (shares the QSPI CLK/D0/D1 lines) | 6 |

## Machine setup (already done on this Mac)

- **ESP-IDF v6.1:** a shallow clone at `~/esp/esp-idf`. Tools are in `~/.espressif` (installed with `./install.sh esp32c6`). Its Python env is `~/.espressif/python_env/idf6.1_py3.14_env`.
- **Homebrew packages:** `cmake ninja dfu-util ccache`.
- **Python:** ESP-IDF needs ≥3.10, and macOS `/usr/bin/python3` is 3.9. That's why `get_idf` prepends `/opt/homebrew/opt/python@3.14/libexec/bin`.
- **`~/esp/projects/`:** the original working copies (`hello_world`, `ws_lvgl`, `devdeck`). `ws_lvgl` is Waveshare's LVGL v9 demo, patched to build on IDF 6.1, and is useful as a hardware smoke test. **This repo is now the source of truth for devdeck.**
- **`~/esp/tools/`:** the earlier copy of the laptop tools. Superseded by `tools/` here.
- **`~/notes/esp32-devdeck/README.md`:** an earlier dated note. This file supersedes it.

## Repo layout

```
firmware/devdeck/
  CMakeLists.txt          project; relaxes 2 C++ warnings the Waveshare BSP trips under IDF 6
  sdkconfig.defaults      16 MB flash, LVGL 64 KB pool, Montserrat 12–48
  partitions.csv          nvs 24K, phy, factory 6M, spiffs 3M (unused)
  main/
    main.cpp              boot: pick app → PMU → panel → touch → LVGL (per-app strip) → app → USB link; BOOT button
    app.h                 shared C interface (link, LVGL lock, app registry)
    apps.c                app registry, NVS app id, crash fallback, apps_switch()
    home.c                launcher grid
    link.c                USB JSON-lines reader, dispatch to the running app, framebuffer snapshot
    devdeck.c             (was ui.c) tileview, page dots, autoplay, shared widget helpers, topic dispatch
    painting.c            Living Painting
    audio.c/.h            ES7210 mic bring-up (I2S TDM + esp_codec_dev)
    lens.c                Sound Lens
    radio.c/.h            listen-only Wi-Fi helpers
    field.c               Signal Field
    ghost.c               Ghost Sense
    screens.h             palette + per-screen create/on functions
    scr_orbit.c           Agent Orbit
    scr_context.c         Context
    scr_pipeline.c        Deep Work
    scr_pulse.c           Ship Pulse
    idf_component.yml     managed deps (LVGL, adapter, panel/touch drivers, cJSON)
    user_config.h         Waveshare's pin header (kept for reference; see pin map note)
  components/             Waveshare BSP, patched for IDF 6.1
    port_bsp/             I2C bus, QSPI panel (SH8601 init sequence), touch init
    app_bsp/              esp_lvgl_adapter setup + 2-pixel-aligned rounder (CO5300 needs even areas)
    pmicpower/            AXP2101 setup (wraps vendored XPowersLib, MIT)
tools/
  deck.py                 bridge / send / snap / shot / app
  serlog.py               serial reader that survives USB re-enumeration
docs/screenshots/         on-device framebuffer captures (2026-10-11)
```

Resolved component versions (`dependencies.lock`, gitignored):

| Component | Version |
|---|---|
| lvgl/lvgl | 9.6.0~1 |
| espressif/esp_lvgl_adapter | 0.3.2 |
| espressif/esp_lcd_sh8601 | 1.0.0 |
| waveshare/esp_lcd_touch_cst9217 | 1.0.4 |
| espressif/esp_lcd_touch | 1.2.1 |
| espressif/cjson | 1.7.19~2 |

## Architecture

```
laptop                                              board
───────────────────────────────                     ──────────────────────────────────────
~/.claude/projects/**/*.jsonl ─┐                    link.c (task "link")
gh api graphql / gh search ────┼─► deck.py bridge ─►  JSON line → cJSON → ui_on_message()
                               │   (USB serial)        under LVGL lock → scr_*_on(topic)
                               │                     LVGL task (esp_lvgl_adapter, prio 10)
deck.py shot ──SIGUSR1──► bridge ── {"t":"snap"} ──►   FLUSH_START hook streams each strip
              ◄── shot.png ◄── @@SNAP strips ◄─────   back as raw RGB565
```

### Wire protocol (USB-Serial/JTAG, one JSON object per line)

**Host → device:**

| `t` | Payload | Effect |
|---|---|---|
| `agents` | `s:[{p,n,live}]`, `r` | Orbit roster (stable slot order, max 6). `n` = calls in the last 10 min, `live` = a call in the last 90 s, `r` = calls/min |
| `tool` | `s` (slot), `n` (tool), `d` (detail) | One tool call: spark plus ticker line |
| `ctx` | `focus`, `s:[{p,u,max,cr,m,h[],cmp}]` | Context gauge data. `u` = tokens in the context, `cr` = cache-read tokens, `h` = last 32 per-turn sizes |
| `pipe` | `c[8]`, `last`, `skill`, `ago`, `total` | Deep Work stage counts |
| `pulse` | `total`, `streak`, `lv` (levels, week-major), `d14[]`, `prs:[{r,ti,st}]` | Ship Pulse |
| `tile` | `i` | Jump to screen `i` (debug/screenshot use) |
| `app` | `id` | Acked, then the board reboots into that app |
| `commit` | `h` (short hash), `r` (repo) | Living Painting seeds a bloom; the bridge sends it when HEAD moves in a live session's repo (polled every 20 s) |
| `snap` | | Stream a framebuffer snapshot |
| `ping` | | Acked only |

**Device → host:** `@@APP <id>` once per boot. `@@ACK <t>` per message. A snapshot is `@@SNAP x1 y1 x2 y2 nbytes\n` followed by raw RGB565 for each strip, then `@@SNAPEND`. Normal `ESP_LOG` output is interleaved, and the bridge drains and ignores it.

### Firmware notes

- **Memory:** no PSRAM, and a full frame (460 KB) doesn't fit. LVGL renders partially into two 480×100 strips (about 192 KB) through `esp_lvgl_adapter`. Static decoration (stars, rings, heatmap cells) is drawn in `DRAW_MAIN` callbacks instead of being created as objects, to save LVGL heap. Boot log: **free heap 228,900 B, largest block 208,896 B**.
- **Threading:** LVGL runs in the adapter's task. The link task wraps every `lv_*` call in `app_lock()`. All UI code is C. The BSP is C++, so `main.cpp` exports `extern "C"` lock shims.
- **Snapshot:** a `LV_EVENT_FLUSH_START` hook copies `lv_display_get_buf_active()->data` for each strip while a `snapping` flag is set, with a forced full invalidate and `lv_refr_now`. It captures the **framebuffer before the panel**, so it can't detect panel-level color or orientation problems.
- **USB writes are chunked to 1 KB.** `usb_serial_jtag_write_bytes` silently rejects anything larger than its TX ring buffer.
- **Autoplay** (`ui.c`): when `lv_display_get_inactive_time()` > 60 s, the screen advances every 15 s.

### Bridge notes (`tools/deck.py`)

- **Transcript tailing:** reads only new complete lines, and backfills the last 400 KB of each file on first sight. Subagent transcripts (`<session>/subagents/*.jsonl`) count toward their parent session. A session drops out after 20 min of silence.
- **Labels:** each session is named after the basename of its transcript's `cwd`, with `~` for the home directory and `#2`… suffixes for duplicates. So a session's label follows its *current* working directory.
- **Context size** = `input_tokens + cache_read_input_tokens + cache_creation_input_tokens` from the latest assistant message.
- **Slow feeds** (`pipe`, `pulse`) refresh every 10 min on a background thread. That keeps a slow `gh` call from stalling the sparks. Only changed messages are resent, and everything is resent after a reconnect.
- **Deep-work stage mapping:** the regexes in `STAGES` cover the old `dw-0N-*` skill names, the newer `create-*`/`iterate-*` ones, and the superpowers equivalents. Slash-command invocations (`<command-name>/x</command-name>`) count too.

## Verification status

**Verified this session.** Evidence is the framebuffer snapshots in `docs/screenshots/` plus serial logs:
- The repo build compiled clean on ESP-IDF v6.1 and was flashed. The boot log shows no crashes and 228,900 B free.
- All four screens render with live data, verified through snapshots taken by the bridge running from this repo.
- Orbit sparks and the ticker react to real tool calls within about 1 s; the ticker showed this session's own commands.
- Autoplay worked: after 75 s idle, two captures 16 s apart showed Context and then Deep Work, with no commands sent.
- The PMU, panel init and touch controller all come up. The touch controller answered with chip ID `0x9220` and 480×480.

**Not verified (needs eyes and hands):**
- Colors and orientation on the physical panel. The earlier question about the Waveshare widgets demo was never answered.
- Swipe between tiles, and tap-to-cycle on Context.
- Long-running stability. The longest continuous run was well under an hour.

## Decisions

| Decision | Why |
|---|---|
| ESP-IDF 6.1 (not Arduino, not MicroPython) | Full C6 support (LP core, Thread/Zigbee), LVGL 9 through the component manager. Cost: porting Waveshare's 5.5 examples (see gotchas) |
| Reuse the Waveshare BSP, write the app in C | The panel init sequence and PMU power sequencing are fiddly and already proven. C avoids C++'s friction with LVGL enums and initializers |
| USB bridge, not on-device data fetching | The data (transcripts, `gh` auth) lives on the laptop, and USB needs no credentials or radio |
| **Wi-Fi built, then removed** | STA + mDNS (`devdeck.local`) + TCP :7777 worked end to end: same protocol, snapshots over TCP, credentials persisted in NVS. But free heap dropped **228 KB → 61 KB** (largest block 46 KB), and the board lives on USB-C. Removed at the user's request; the NVS partition was then erased and read back to confirm the credentials are gone |
| On-device snapshots | Lets an agent check UI changes without a human looking; it caught several layout bugs |
| Black backgrounds | True black on AMOLED looks better, and it keeps redraw areas small |

If Wi-Fi comes back, shrink its buffers first (`CONFIG_ESP_WIFI_*_BUFFER_NUM`, static RX/TX buffers) and measure heap. Provision credentials into NVS over USB; never put them in source.

## Gotchas found (IDF 6.1 vs Waveshare's 5.5 examples)

1. LVGL 9.6 deprecated `CONFIG_LV_MEM_SIZE_KILOBYTES`, and the deprecation warning fails the build under `-Werror`. Use `CONFIG_LV_MEM_SIZE` (in bytes).
2. GCC 15 rejects LVGL's IRAM section attributes. Drop `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM`.
3. LVGL's benchmark/music demos `#warning` about RAM, which breaks the build under `-Werror`. Disable them.
4. The driver split: use `esp_driver_gpio`, `esp_driver_i2c`, `esp_driver_spi` and `esp_lcd`, not `driver`.
5. Stricter C++ trips the BSP: missing-field-initializer errors and int→`gpio_num_t` conversions. The project `CMakeLists.txt` relaxes both checks, and the casts are patched in `display_bsp.cpp`.
6. cJSON is no longer bundled with IDF. It comes from the managed component `espressif/cjson`.
7. Any chip reset re-enumerates the USB port, so serial readers must reconnect (`serlog.py` does).
8. Montserrat is ASCII-only plus FontAwesome glyphs, so characters like `·` render as boxes.
9. `lv_obj_get_width()` returns 0 before the first layout pass. Position from `lv_obj_get_style_width()`, or use flex containers.
10. LVGL `LONG_MODE_DOTS` only truncates when the label's height is fixed.

## Known issues / limitations

- **First hardware run (2026-10-11), screenshots in `docs/screenshots/launcher/`.** All six apps boot without panics and switching works. Free heap after start: home/devdeck 117 KB, painting 39 KB, lens 27 KB (tight), field 100 KB with all three radios up (63 frames/s, 33 devices), ghost 116 KB.
- **Sound Lens** first failed with "out of memory": its 67 KB waterfall didn't fit LVGL's 64 KB pool, so the pixels now come from the system heap. The quiet-room waterfall glowed at the low end; the mic's DC offset is now removed per block and the display floor raised 6 dB (not yet re-checked on hardware).
- **Ghost Sense gets no data on the home network.** The router beacons in 802.11b, which carries no CSI, and other CSI-bearing frames arrived at about 1/s across all transmitters, so the format lock (30 frames) and the fallback (10 frames in 3 s) never trigger. The usual fix is to join the network and ping the router for steady replies; that needs Ron's go-ahead because it means storing Wi-Fi credentials on the board.

- **The bridge isn't a service.** It runs only while a terminal or session keeps it alive. When it stops, the board keeps showing its last data with no "stale" indicator. One possible fix is a launchd agent plus a staleness badge on the device.
- **False compaction counts.** The Context screen counts any drop of more than 40% between turns as a compaction, so a one-turn spike followed by a normal turn is misread (visible in `2-context.png`). A fix is to require the drop to persist, or to read the transcript's compaction marker.
- **The context window is guessed.** The bridge assumes 200K, or 1M when the model name contains `1m` or usage exceeds 200K. A real per-model window lookup would be better.
- **Deep Work's "last" can look stale.** It's correct, but shows 153 days ago: no deep-work skills have been run recently. Ticket shows 0 because `refine-ticket` hasn't run.
- **`gh search prs --author=@me`** only sees github.com, so work PRs on other hosts don't appear.
- **`deck.py shot <tile>` can race autoplay** and capture a half-scrolled frame. This only affects debug captures.
- **Vendored Waveshare BSP files** (`components/port_bsp`, `app_bsp`, `pmicpower/*.cpp`) come from a repo with no license file. **Check that before making this repo public.** XPowersLib (`pmicpower/src`) is MIT and keeps its headers.

## Ideas not started

- **IMU** (QMI8658; Waveshare's code references both I2C 0x6A and 0x6B, so confirm with a bus scan): tilt for parallax on Orbit, shake to force a feed refresh. Waveshare example `02_I2C_QMI8658`.
- **Mics:** voice push-to-talk to a Claude session (Waveshare ships a XiaoZhi voice-assistant example). Needs the audio codec bring-up.
- **PMU readouts:** battery level and charge state on a status strip, using the AXP2101 helpers already in `power_bsp.cpp`.
- **More screens** fed by the same bridge: herdr pane states, PR review queue, CI status for active branches.
- **Similar community projects worth reading:** `mfellner/esp32-playground` (a multi-app launcher for this board) and `Keung123/ESP32-Codex-Sidekick` (a coding-agent companion on the same board).
