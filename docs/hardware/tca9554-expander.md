# TCA9554 — the IO expander

I2C `0x20`. Small, dumb, and load-bearing: **it gates the panel's power
rail** and both reset lines, none of which reach a GPIO on this board.

## Sequence

```
config reg 0x03 ← 0xF8      # EXIO0..2 outputs, EXIO3..7 left as inputs
output reg 0x01 ← 0x00      # all three low
wait 20 ms
output reg 0x01 ← 0x07      # all three high
wait 20 ms
```

That is `releaseResets()` in `main/panel.cpp`, and its mirror `cutPower()`
writes `0x01 ← 0x00`. Those two are the **only** writes that move these
bits, and they run on every screen sleep/wake, not just at boot — which is
why they return errors instead of aborting: a transient NAK here must not
reboot an accessory that is live in Apple Home.

All three bits low is also the correct low-leakage state for pins whose
supply has gone.

## EXIO map — and an unresolved disagreement

| EXIO | Function | Agreed? |
|---|---|---|
| 0 | LCD_RST | yes |
| **1** | **TP_RST** *or* **DSI_PWR_EN** | **no — see below** |
| **2** | **DSI_PWR_EN** *or* **TP_RST** | **no — see below** |
| 5 | AXP_IRQ | from `../NOTES.md`; never read by this firmware, which polls the PMU over I2C instead |

Two sources in this project disagree about EXIO1 vs EXIO2:

- **`../NOTES.md` (I2C map section)**: `EXIO0=LCD_RST, 1=TP_RST,
  2=DSI_PWR_EN`.
- **`docs/plans/idf-port.md` board-facts table**, and the comment at the top
  of `releaseResets()` in `main/panel.cpp`: `EXIO0=LCD_RESET,
  EXIO1=DSI_PWR_EN, EXIO2=TP_RESET`, asserting that the schematic and the
  vendor BSP agree on this and that the older note has 1 and 2 swapped.

**Unresolved.** The port-era claim cites the schematic and BSP and is the
more likely of the two, but nobody has re-derived it since, and one source
asserting the other is wrong is not the same as a check.

**Nothing depends on it today.** `releaseResets()` drives all three bits
high together and `cutPower()` drives all three low together, so the mapping
never affects behaviour — which is exactly why it has stayed unresolved, and
also why it is a trap waiting for the first person who wants to move one bit
alone. Two things that would need it settled:

- Task #22 (wake-on-touch): keeping the panel rail up while holding touch in
  reset, or vice versa, requires knowing which bit is which.
- Any attempt to distinguish "rail down" from "held in reset" when the
  digitiser NACKs — see [cst820-touch.md](cst820-touch.md).

Settle it with a bench check (drive one bit, see whether the panel or the
digitiser reacts) or by reading the schematic again, and record the answer
here.

## Why it matters

`DCDC1 3.3 V → VCC3V3 → DSI_PWR_EN (this chip) → VCI → CO5300 panel + CST820
touch.` This expander bit is the on/off switch for the display module and
the digitiser together, and dropping it is the largest single power saving
available on this board. See [board.md](board.md) for the full topology and
[co5300-display.md](co5300-display.md) for the ~261 ms cost of turning it
back on.
