# sdgs

![SDGs badge demo](docs/demo.gif)

A tiny display toy: an **ESP32-C3** drives a **1.28" round LCD (GC9A01A)** that renders the
17-colour SDGs wheel, spinning, with `SDGs` in the centre. The spin, the centre label and a photo
can be changed from a phone over Bluetooth.

The wheel is drawn by hand — no image assets:

- each colour wedge is two filled triangles (`drawSDGsArc`),
- the ring is repainted **in place** every frame (the screen is never cleared) so it doesn't flicker,
- the centre label is drawn **last** so the wheel can't overwrite it.

## Phone remote (Bluetooth)
Open **https://aideahandesen-dvs.github.io/sdgs/** (served from `docs/index.html`) in **Chrome on Android**
(Web Bluetooth; iPhone Safari doesn't support it) and tap *connect*.
The badge advertises as `sdgs`. From the page you can:

- spin clockwise / anticlockwise, stop, and set the speed (degrees per frame, 1–30; the original was 8),
- change the centre label (ASCII, up to 24 chars), its size (1–8) and colour.
  If the label doesn't fit inside the ring, the badge shrinks it automatically,
- send a photo: crop it on the phone (drag / zoom) and show it either **inside the ring** (164×164, the wheel keeps spinning around it)
  or **full screen** (240×240, no wheel). Sent as RGB565 in 480-byte chunks (164×164 ≈ 54 KB, 240×240 ≈ 115 KB; transfer time not measured yet),
- switch between *wheel + label*, *wheel + image* and *image only*.

Settings are saved in NVS and images in LittleFS, so they survive a power cycle.
The centre and full-screen images are stored separately, so switching doesn't need a resend.

| UUID | | |
|---|---|---|
| `5d650001-…` | service | |
| `5d650002-…` | spin | 3 bytes: speed, direction (0 = CW, 1 = CCW), run (0/1) |
| `5d650003-…` | label | ASCII, ≤ 24 bytes |
| `5d650004-…` | style | write 3 bytes: size, colour (RGB565 LE) · read adds the size actually used |
| `5d650005-…` | show | 1 byte: 0 = wheel + label, 1 = wheel + centre image, 2 = image only |
| `5d650006-…` | image | write `1, kind` (0 = centre, 1 = full) to begin · `2, offset (u32 LE), data…` · `3` to finish. Read: has centre, has full, last result (0 ok / 1 failed / 2 in progress) |

(full UUIDs: `5d65000N-2f67-465b-9d3c-33c4607b0907`)

### How it fits together
- **BLE callbacks never draw.** Labels and styles are copied into a buffer under a spinlock and picked up by `loop()`;
  NVS writes also happen in `loop()`.
- **Images** are written by the BLE callback straight into `/upload.tmp` on LittleFS, chunk by chunk.
  Each chunk carries its offset; a gap or mismatch marks the upload as failed. On *finish*, if the size is right,
  `loop()` renames it to `/center.bin` or `/full.bin`, so a half-sent image never replaces a good one.
- **Nothing is cleared every frame.** The centre (label background or image) is repainted only when it changes,
  and the centre image is clipped to a circle row by row so its corners never touch the wheel.
  In *image only* mode the screen is drawn once and left alone.
- The page reads every value back on connect, so it always shows what the badge is actually doing.

## Hardware
- **ESP32-C3** board (e.g. ESP32-C3 SuperMini)
- **GC9A01A** 1.28" 240×240 round SPI LCD

### Wiring (SPI)
| ESP32-C3 | LCD  |
|----------|------|
| GPIO1    | RES  |
| GPIO2    | DC   |
| GPIO4    | SCLK |
| GPIO6    | MOSI |
| GPIO7    | CS   |
| 3V3      | VCC  |
| GND      | GND  |

## Build
Uses [PlatformIO](https://platformio.org/):

```
pio run -t upload
```

## Notes
- Uses the BLE stack bundled with the Arduino core. The external `NimBLE-Arduino 1.4.3` crash-loops in `esp_bt_controller_init` on the current core.
- The first boot after flashing logs `Corrupted dir pair` / `mount failed` from LittleFS: the partition is blank, and it gets formatted right after. Later boots are quiet.
- The badge keeps advertising while connected, so the phone can reconnect even if a stale connection lingers.
- Blank screen? Read the serial log first (`pio device monitor`); decode crash addresses with
  `riscv32-esp-elf-addr2line -pfiaC -e .pio/build/esp32-c3-devkitm-1/firmware.elf <addr>`.

## Status
- Verified on the badge: builds, flashes and boots cleanly with BLE + LittleFS (serial log, no crash or reboot loop);
  the page is live on GitHub Pages.
- **Not yet tried from a phone:** connecting, spin / label / style changes, image upload (and its speed),
  switching display modes, settings surviving a power cycle.
- Ideas: brightness (needs the backlight pin wired), Japanese text (the built-in GFX font is ASCII only).

## License
MIT
