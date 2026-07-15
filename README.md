# CYD 3.5" Now-Playing Controller

A now-playing display and remote control for [pear-desktop](https://github.com/pear-devs/pear-desktop)'s `api-server` plugin, running on a 3.5" "Cheap Yellow Display" ESP32 board (**ESP32-3248S035R** — ST7796 SPI display + XPT2046 resistive touch, shared bus).

![Device running the now-playing UI](assets/device.jpg)

## Features

- Title / artist, album art, and playback position, updated in real time over the `api-server` websocket (no polling)
- Touch controls: previous / play-pause / next
- Vietnamese and other Unicode text support (u8g2 `unifont` font), with marquee scrolling for titles/artists that don't fit
- Background/accent color derived from the current track's album art, computed on the desktop side and sent over the websocket
- Album art is pre-scaled and re-encoded on the desktop before being pushed to the device — the ESP32 only decodes a small, already-sized JPEG, it never fetches or resizes the original thumbnail itself
- Touch calibration is stored in NVS and only runs once, on first boot

## Hardware

- ESP32-3248S035R ("CYD" 3.5", resistive touch, ST7796 + XPT2046 on shared SPI)
- USB cable for flashing (COM port with an ESP32 in download mode)

## Requirements

- [pear-desktop](https://github.com/pear-devs/pear-desktop) running with the **api-server** plugin enabled (Options → Plugins → API Server), reachable from the ESP32 over Wi-Fi
- [PlatformIO](https://platformio.org/) (CLI or the VS Code extension)

## Setup

1. Copy the secrets template and fill in your Wi-Fi and api-server details:
   ```bash
   cp src/secrets.h.example src/secrets.h
   ```
   ```cpp
   #define WIFI_SSID "your-wifi-ssid"
   #define WIFI_PASS "your-wifi-password"
   #define API_HOST  "192.168.1.10"   // IP of the machine running pear-desktop
   #define API_PORT  26538
   #define API_BASE  "http://192.168.1.10:26538"
   ```
2. Build and flash:
   ```bash
   pio run -t upload --upload-port COM7
   ```
   If upload fails with "Wrong boot mode detected", hold the board's **BOOT** button, tap **RST**, keep BOOT held, then retry the upload — release BOOT once it starts connecting.
3. On first boot, touch the four calibration targets when prompted. This only happens once; the result is saved to flash.

## How it works

- `src/main.cpp` connects to `api-server`'s `/api/v1/ws` for live song/position/state updates, and posts to `/api/v1/previous`, `/api/v1/toggle-play`, `/api/v1/next` for controls.
- Album art and accent color aren't fetched by the device: pear-desktop's `api-server` plugin (see its `backend/routes/websocket.ts`) resizes the art to the device's exact display size, re-encodes it as a small JPEG, computes an average "signature color", and pushes both over the same websocket connection — as a binary frame for the art, and as an `accentColor` field alongside the normal JSON player-state messages.

## License

MIT
