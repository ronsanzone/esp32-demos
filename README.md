# esp32-demos

Demos for the Waveshare ESP32-C6-Touch-AMOLED-2.16, a 480×480 AMOLED touch board.

**devdeck** turns the board into a live dashboard of your dev process. The laptop sends data over USB to four screens:
- **Agent Orbit:** live Claude Code sessions and their tool calls
- **Context:** context-window usage for each live session
- **Deep Work:** your deep-work pipeline stages
- **Ship Pulse:** GitHub contributions and open PRs

![devdeck](docs/screenshots/overview.png)

To build and flash, load ESP-IDF 6.1 (`get_idf`), then:

```bash
cd firmware/devdeck && idf.py build && idf.py -p /dev/cu.usbmodem101 flash
```

To feed it live data, set up the tools once:

```bash
cd tools && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
```

Then start the bridge, and stop it before flashing:

```bash
tools/.venv/bin/python tools/deck.py bridge
```

See **[HANDOFF.md](HANDOFF.md)** for the hardware, architecture, wire protocol, status, decisions and known issues.
