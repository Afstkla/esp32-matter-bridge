# Hardware knowledge base

One file per chip or subsystem on this board. Everything here was learned
from the schematic, a datasheet, or the bench — where it matters, which one
is stated.

| File | What it covers |
|---|---|
| [board.md](board.md) | The device itself: SoC, memory, USB-Serial-JTAG (and its wedge), buttons, I2C bus, power-rail topology, full pin map |
| [co5300-display.md](co5300-display.md) | The AMOLED controller: column offset, reset path, one-transaction rule, sleep modes and their real costs |
| [cst820-touch.md](cst820-touch.md) | The digitiser: part-number confusion, register bring-up, shared power rail, INT on GPIO21 |
| [axp2101-pmu.md](axp2101-pmu.md) | The PMU: PEK button, charging fix, what it can and cannot measure, rails |
| [tca9554-expander.md](tca9554-expander.md) | The IO expander that gates the panel rail and both reset lines |
| [es8311-audio.md](es8311-audio.md) | Codec, amplifier, I2S pins — brought up and measured, including the amplifier's 175 ms wake |
| [qmi8658-imu.md](qmi8658-imu.md) | IMU — present, unused |
| [pcf85063-rtc.md](pcf85063-rtc.md) | RTC — present, unused |

Deeper pipeline lore lives next door and is not duplicated here:
[../display-notes.md](../display-notes.md) (DMA/flush path, underflow
semantics), [../matter-notes.md](../matter-notes.md) (Matter serve paths),
[../../components/esp_lcd/PATCH.md](../../components/esp_lcd/PATCH.md) (why
the vendored driver exists).

## The rule

**Read your peripheral's file before debugging it. Write new findings back
in the same task** — a finding that only lives in a report or a chat is a
finding the next agent pays for again. **Disproven theories stay, labeled**
with their refutation; this repo's history is littered with
plausible-but-wrong stories that cost days, and deleting them invites the
next person to re-derive them.

Anything marked *datasheet only* or *unbenched* has not been measured on
our unit. Say so when you add such a claim, and upgrade the label when a
bench check settles it.
