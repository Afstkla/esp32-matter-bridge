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

Today's `panelSleep()` deliberately does **not** send SLPIN: the driver's
`disp_on_off` only sends DISPOFF, so SLPIN would be a raw QSPI command, and
it saves nothing once VCI is gone and the reset lines are low. That
reasoning is correct *for the rail-cut design* and stops applying the moment
the rail stays up.

Sleep order that works: black the framebuffer, brightness 0, DISPOFF, 20 ms
settle (the module's decoupling bleeding down), then drop the expander bit.

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
