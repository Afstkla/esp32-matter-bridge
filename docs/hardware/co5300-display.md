# CO5300 — the AMOLED controller

368×448 RGB565 AMOLED over QSPI. Driven by `espressif/esp_lcd_co5300`
(^2.1.0) from `main/panel.cpp`. This is the **V2** part; V1 boards carry an
SH8601 and nothing here applies to them.

For the DMA/flush pipeline — chunk arithmetic, underflow detector semantics,
PM interaction, the open black-rows bug — read
[../display-notes.md](../display-notes.md). It is not repeated here.

## Geometry and wiring

- **Column offset is 16.** `esp_lcd_panel_set_gap(panel, 16, 0)`, straight
  from Waveshare's own V2 example. Guessing 0 gives a horizontally shifted
  image, which cost real debugging time.
- QSPI: CS=12 SCK=11 D0=4 D1=5 D2=6 D3=7, on SPI2 (SPI3 cannot DMA out of
  PSRAM on this chip).
- **40 MHz, not 80.** At 80 MHz every flush ends in "DMA TX underflow" and a
  black panel once the framebuffer lives in PSRAM under `esp_lcd`. 40 MHz is
  20 MB/s, just above the ~18 MB/s the old Arduino build actually sustained.
- Framebuffer: full frame in PSRAM, 64-byte aligned (so the SPI driver DMAs
  it as it stands instead of bouncing it), **big-endian RGB565** — that is
  what the wire wants, nothing byte-swaps on the way out.
- Brightness is register 0x51 on the 0–255 scale, sent as a raw QSPI command
  word (`0x02 << 24 | 0x51 << 8`). On-brightness here is 180.

## Reset comes from the expander, not a GPIO

The panel is created with `reset_gpio_num = -1`. LCD_RST hangs off the
TCA9554, so `esp_lcd_panel_reset()` only works because the expander pulse
(low, 20 ms, high, 20 ms) has already run. See
[tca9554-expander.md](tca9554-expander.md).

## Never blit per-row

**One `draw_bitmap` for the whole frame.** The CO5300 leaves the panel
*black* if a frame arrives as many small, separately addressed writes — a
QSPI state issue. The symptom is nasty: firmware runs perfectly, the
framebuffer contents are correct (`screendump` proves it), the screen just
stays dark.

`esp_lcd` chunks the frame internally into 11 transactions but keeps CS
asserted across them, which is the shape that works.

## One-shot CASET/RASET, then RAMWR-continue

`esp_lcd_co5300` sends CASET (0x2A) and RASET (0x2B) **once** per frame,
then **one** RAMWR (0x2C) that spans all 11 chunks under a single CS
assertion (`SPI_TRANS_CS_KEEP_ACTIVE` on every chunk but the last). The
GRAM write pointer auto-increments across chunk boundaries.

This is the discriminator that makes display corruption diagnosable:

- A **skipped** chunk leaves stale pixels from the previous frame, or a
  torn/shifted frame if the pointer desyncs — **never** a clean black band.
- A clean black band means zeros were **transmitted**, on schedule, as
  ordinary pixel data. The panel did exactly what it was told.

## Sleep: three different things, three different costs

| State | What it is | Cost |
|---|---|---|
| Brightness 0 | Register write only | **Not off.** The CO5300 keeps its charge pump running for ELVDD/ELVSS whatever it is showing. On an AMOLED the black pixels are unlit, so it is a real saving — but the controller is still fully alive |
| SLPIN (0x10) | Controller standby, booster off, ~120 ms to settle | **~110–135 µA total** (VCI 25 µA + VDDI 85–110 µA), CO5300 datasheet V0.00 §6.2, **datasheet only — never measured on our unit** |
| Rail cut (DSI_PWR_EN low) | VCI gone; module unpowered | Whatever the module draws, which the light-sleep hunt concluded was the larger half of the shelf drain. Costs a **~261 ms cold start** to undo, and takes touch with it |

**Disproven: "SLPIN costs tens of mA."** That fear came from measuring a
**lit** panel, not a panel in SLPIN. The datasheet number above is three
orders of magnitude smaller. The consequence matters for task #22
(wake-on-touch): "rail up + SLPIN + touch standby" is plausibly µA-cheap,
which is the opposite of the assumption the current rail-cut design was
built on. Still unverified — the one number nobody has is the module's own
upstream regulator overhead, and only a battery-slope soak can supply it.

Both ship, as the two tiers of one sleep: `panelDoze()` (SLPIN, rail up) for
the first minute after the screen goes dark, `panelSleep()` (rail cut) after
that. `panelSleep()` still does **not** send SLPIN — the driver's `disp_on_off`
only sends DISPOFF, and SLPIN saves nothing once VCI is gone and the reset lines
are low.

Sleep order that works: black the framebuffer, brightness 0, DISPOFF, 20 ms
settle (the module's decoupling bleeding down), then drop the expander bit.

### The driver has no sleep call, and the API it would use is left null

`esp_lcd` defines a `disp_sleep` panel op and exports
`esp_lcd_panel_disp_sleep()`, but `espressif/esp_lcd_co5300` never assigns it
— `esp_lcd_co5300_spi.c` wires up `disp_on_off` (DISPON `0x29` / DISPOFF
`0x28`) and nothing else, so `esp_lcd_panel_disp_sleep()` returns
`ESP_ERR_NOT_SUPPORTED`. SLPIN and SLPOUT go out as raw command words the same
way brightness does:

```c
static const uint32_t QSPI_CMD_SLPIN  = (0x02u << 24) | (0x10u << 8);
static const uint32_t QSPI_CMD_SLPOUT = (0x02u << 24) | (0x11u << 8);
```

There is no readback: `panel_io_spi_rx_param()` sends its command phase
single-line and never sets the quad flags, so RDDPM (`0x0A`) cannot be used to
confirm the sleep bit on a QSPI panel. **Whether the picture comes back
correctly is an eyes-on check, not a machine-checkable one.**

### Rail-up sleep measured: 29 ms down, 139 ms back

`panelDoze()` / `panelRouse(settleMs)` are the rail-up pair — black frame,
brightness 0, DISPOFF, SLPIN on the way down; SLPOUT, settle, DISPON, flush,
brightness on the way back. `doze <0|1> [settle-ms]` drives them. Measured on
our unit, same binary, same session:

| Path | Cost |
|---|---|
| `panelDoze()` | **29 ms** (one 18 ms black flush plus two commands) |
| `panelRouse(120)` | **139 ms** — the datasheet-safe settle |
| `panelRouse(60)` | **79 ms** — the settle the vendor init sequence itself uses after `0x11` |
| `panelRouse(5)` | **24 ms** |
| `panelWake()` (rail cut, full re-init) | **260–261 ms** |

The fixed cost of the rouse is **19 ms**; everything above that is the settle,
which is the only knob. So the honest headline is **139 ms vs 261 ms — 122 ms
saved, 1.9×** at the settle the datasheet allows, and 3.3× if 60 ms turns out
to be enough. **The short settles are unverified visually** — timing says the
commands went out, nothing here says the panel came back clean at 5 ms. Bisect
the settle with eyes on the screen before shipping anything below 120.

What is genuinely cheaper about this path is not the 122 ms. It is that VCI
never drops, so the controller keeps its configuration and its GRAM, no
SWRESET/MADCTL/COLMOD/vendor sequence runs, **and the CST820 on the same rail
stays alive** — which is the entire reason wake-on-touch is possible at all.

The settle that ships is `PANEL_SLPOUT_SETTLE_MS` in `main/panel.h`, **120 ms**,
the datasheet-safe one — one constant, used as the default of the one runtime
knob (`doze <0|1> [settle-ms]`, which drives the shipped path rather than a
bench copy of it). A rouse also has to revive the digitiser, so the tier-1 wake
is the 139 ms plus a TP_RESET pulse and the touch retry loop (~40–60 ms when the
part answers at once, as it does after a rail-cut wake).

## Waking is a full re-init — ~261 ms

VCI going away means the controller forgets everything: wake runs SWRESET,
MADCTL/COLMOD, the vendor sequence (SLPOUT among it) and DISPON. Roughly
180 ms of that is the CO5300's own mandated delays. `panel wake took N ms`
is logged on every wake; measured ~260–261 ms.

The black framebuffer goes out **before** brightness comes up, so the first
lit frame is not whatever the GRAM powered up holding. Touch is initialised
last — it costs the longest, and nobody can tap a screen they cannot see.

A wake that fails logs, re-cuts the rail and stays asleep so the next PWR
press simply tries again. Aborting instead would reboot an accessory that is
live in Apple Home over one NAK on a bus the PMU also uses.

## Arduino-era note, still true

`Arduino_CO5300::displayOff()` sends DISPOFF **and then SLPIN**. On this
board that killed touch (shared rail) with no way back — see
[cst820-touch.md](cst820-touch.md). Irrelevant to the IDF firmware, which
never calls it, but the trap is the board's, not the library's.

## Disproven: the transaction queue depth was not the wedge fix

`io.trans_queue_depth = 12` (twelve descriptors, one more than a frame's
eleven chunks, so no chunk ever waits on a mid-frame recycle) was once
believed to fix the ui task wedging inside `draw_bitmap`. **It did not.**
The real mechanism was `esp_lcd` losing count of a faulted-but-consumed
transaction result — see
[../../components/esp_lcd/PATCH.md](../../components/esp_lcd/PATCH.md). The
depth is kept because it is correct, not because it fixed anything.
