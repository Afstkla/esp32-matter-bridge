# AXP2101 — the power management unit

I2C `0x34`, chip ID `0x4A` in register `0x03`. Driven by raw register access
from `main/keys.cpp` — no XPowersLib dependency; that library's source is
used only as a register-map reference.

## The PWR button is a PMU interrupt, not a GPIO

PWR (the **bottom** button) reaches the PEK pin, not a GPIO. The pattern:

1. Enable the short-press IRQ — INTEN2 (`0x41`) bit 3.
2. Poll INTSTS2 (`0x49`) bit 3 for the flag.
3. Clear the status by writing `0xFF` to INTSTS1/2/3 (`0x48`/`0x49`/`0x4A`).

**Only the short-press IRQ is enabled.** Long-press is the PMU's own
power-off and is deliberately left alone — taking it over would cost the
only way to hard-power-down a board with a battery attached (hold ~10 s).
The latch is why a PWR press is never missed: it sits in the status register
until something reads it, which is what makes it a reliable wake from screen
sleep.

## The TS-pin charging fix

`keysInit()` reprograms two registers at startup, and both are load-bearing:

- **TS_PIN_CTRL (`0x50`)**: low nibble forced to `0x10`, "external input".
  This board has **no TS thermistor wired**, and leaving the pin at its
  reset default (NTC input) lets the charger read a bogus temperature off a
  floating pin and **refuse to charge**. Fixing it takes the TS pin out of
  the charge-safety decision entirely. XPowersLib's `begin()` does the same
  thing on every chip it recognises, which is why the Arduino firmware never
  hit this and the early IDF port could have.
- **ADC_CHANNEL_CTRL (`0x30`)**: bit 1 cleared, disabling the TS ADC channel
  along with it.

## What it can and cannot measure

**There is no current ADC.** The AXP192 had one; the AXP2101 does not.
`pmuReport()` gives battery mV, VBUS mV, system mV, battery percent, charge
flags and die temperature — nothing more. **Drain is therefore only
measurable as a battery-voltage slope over time**, which is what `battlog`
exists for: a 144-entry NVS ring sampled every 10 minutes, giving a rolling
24-hour window that survives reboots. For an absolute figure on the bench,
use an inline USB power meter; the device cannot weigh its own consumption.

ADC register widths differ: battery voltage is 13-bit (high byte masked
`0x1F`, regs `0x34`/`0x35`); VBUS, SYS and die temperature are 14-bit (high
byte masked `0x3F`, regs `0x38`/`0x39`, `0x3A`/`0x3B`, `0x3C`/`0x3D`). All
report directly in the target unit. Die temperature converts as
`22.0 + (7274 - raw) / 20.0` (the XPowersLib constant).

### Die temperature is BLIND to panel draw

Die temp was used as the drain proxy during the light-sleep hunt — pinned at
36–37 °C meant something was burning continuously, and it fell off that pin
immediately once the real cause was fixed. It is a genuine instrument **for
chip-side burn only**.

**Positive control, measured:** a fully lit AMOLED moves the die temperature
by **0.3 °C**. So the panel — the largest single consumer on the board — is
essentially invisible to it. Any argument of the form "the temperature did
not move, therefore the display change saved nothing" is wrong by
construction. Battery-slope soak is the only instrument that sees the panel.

## Battery percent comes from an OCV table, not the fuel gauge

The AXP2101's own fuel gauge has **no battery model programmed**: `battlog`
caught it reporting 52% at 3.58 V on a cell that died half an hour later,
and 50% at 4.19 V. Percent is therefore interpolated in firmware from a
resting-OCV curve for a single LiPo cell (3300 mV → 0% … 4150 mV → 100%).

That curve is only honest **at rest**:

- **Under load** the pack sags and the estimate reads **low**.
- **On USB** the cell sits at charge voltage and it reads **high** — a cell
  at 60% shows ~100% while charging.

Raw mV stays the number to trust. Only `battlog` rows **without** the
`usb`/`chg` flags are state of charge; flagged rows are the charger's
voltage, not the cell's. Entries logged before the OCV change carry the fuel
gauge's numbers and are not comparable.

Matter exposes this as a Power Source cluster with `BatPercentRemaining` in
half-percents (a full cell reads 200), with the same caveats.

## Rails

`power rails` (`pmuDumpRails()`) enumerates every regulator. Enable bits
live in three registers:

- **`0x80`** — the five DCDCs, bits 0..4.
- **`0x90`** — ALDO1-4 (bits 0..3), BLDO1-2 (4..5), CPUSLDO (6), DLDO1 (7).
- **`0x91`** — DLDO2 alone, bit 0.

Voltage bytes are printed **raw** for the buck converters, whose scales
differ per rail, and decoded for the LDO group, which is one uniform ladder:
`500 mV + 100 mV × (byte & 0x1F)`.

**The firmware programs no regulator**, so every reading is the PMU's
power-on default — and a rail with nothing on it on this board is still a
rail that is on.

**`DCDC1 = VCC3V3`** — established while tracing the panel supply. That is
the rail the TCA9554's DSI_PWR_EN gates on its way to the panel module's
VCI (see [board.md](board.md)).

### Exonerated: the idle unused rails are not the drain

During the shelf-drain investigation the always-on-but-unloaded rails were a
named suspect. Estimated total for the idle unused rails: **~100–200 µA** —
two orders of magnitude below the ~50–60 mA being chased, and not worth
programming regulators for. The convictions were elsewhere: a stray SoftAP
pinning the Wi-Fi APB lock (~115 mA), then the Wi-Fi lock's irreducible ~25%
residency plus the never-powered-down AMOLED. Note the estimate is a
datasheet-order figure, not a measurement — **the PMU cannot measure it**,
per "no current ADC" above.

## No-battery behaviour is harmless

With no cell attached the PMU reports `batt=0mV` and the firmware derives
`pct=-1` (`pmuReport()`) or 0 (`pmuStatus()`). `powerPoll()` returns early on
a 0 mV reading, so nothing logs and nothing misbehaves. A bench board with no
battery is a perfectly valid state; ignore the numbers.

Separately, a failed I2C read is tracked as a fault flag rather than
returning a plausible "0 mV, not charging" — that is what distinguishes a
bus glitch from a real reading, and it prints `PMU absent` instead of
lying.
