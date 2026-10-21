# sdgs

![SDGs badge demo](docs/demo.gif)

A tiny display toy: an **ESP32-C3** drives a **1.28" round LCD (GC9A01A)** that renders the
17-colour SDGs wheel, spinning, with `SDGs` in the centre.

Everything is drawn by hand in ~90 lines — no image assets:

- each colour wedge is two filled triangles (`drawSDGsArc`),
- the ring is repainted **in place** every frame (the screen is never cleared) so it doesn't flicker,
- the centre label is drawn **last** so the wheel can't overwrite it.

## Phone remote (Bluetooth)
Open `docs/index.html` in **Chrome on Android** (Web Bluetooth; iPhone Safari doesn't support it) and tap *connect*.
The badge advertises as `sdgs`. From the page you can:

- spin clockwise / anticlockwise, stop, and set the speed (degrees per frame, 1–30; the original was 8),
- change the centre label (ASCII, up to 24 chars), its size (1–8) and colour.
  If the label doesn't fit inside the ring, the badge shrinks it automatically.

Settings are saved in NVS, so they survive a power cycle.

| UUID | | |
|---|---|---|
| `5d650001-…` | service | |
| `5d650002-…` | spin | 3 bytes: speed, direction (0 = CW, 1 = CCW), run (0/1) |
| `5d650003-…` | label | ASCII, ≤ 24 bytes |
| `5d650004-…` | style | write 3 bytes: size, colour (RGB565 LE) · read adds the size actually used |

(full UUIDs: `5d65000N-2f67-465b-9d3c-33c4607b0907`)

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
- The badge keeps advertising while connected, so the phone can reconnect even if a stale connection lingers.
- Blank screen? Read the serial log first (`pio device monitor`); decode crash addresses with
  `riscv32-esp-elf-addr2line -pfiaC -e .pio/build/esp32-c3-devkitm-1/firmware.elf <addr>`.

## License
MIT
