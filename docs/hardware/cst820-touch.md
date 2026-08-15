# CST820 — the capacitive touch controller

I2C `0x15`. Driven by raw register access from `main/panel.cpp`.

## What this chip actually is

Waveshare's own product wiki names the V2 touch part **CST820**. Our
firmware, `../NOTES.md` and every comment in `main/panel.cpp` call it
**CST816** — which is not wrong so much as imprecise: CST820 is the same
Hynitron self-capacitive family as CST816/CST816S/CST716, ESPHome's `cst816`
component covers all of them under one driver, and **our 0x15 driver works
as-is** (chip-ID readback `0xA7 → 0xB7`, coordinate registers `0x01..0x06`,
the `0xFA` interrupt-enable bit). The naming is left alone deliberately;
what matters is knowing that datasheet numbers found under "CST816" are for
a *sibling part*, not this one.

**The V1/V2 trap:** V1 boards carry an **FT3168 at 0x38**. Firmware built
for V1 dies at touch init because nothing answers there; firmware built for
V2 finds nothing at 0x38 on a V1 board. Our unit: 0x15 present, 0x38 absent.
Every published ESPHome config for "the 1.8" is V1 and will not work here.

## Bring-up — the two things that make it work

1. **It does not answer at all for a few hundred ms after reset.** Retry the
   chip-ID read (`0xA7`, returns `0xB7`) rather than probing once; the
   firmware retries 25 times at 20 ms.
2. **It reports nothing until interrupt mode is configured: write
   `0xFA = 0x10`** (bit 4, "motion" — what Arduino_DriveBus writes). Without
   it the coordinate registers read fine but never change, which looks
   exactly like broken hardware. `panelTouchDump()` exists precisely to tell
   that state apart from a wiring or address fault.

Registers `0x01..0x06` are GestureID, FingerNum, XposH, XposL, YposH, YposL.
Mask the coordinates with `& 0x0F`; the high nibble of XposH is the contact
flag (0x8 down, 0x4 up). No rotation or axis swap — touch and display share
one coordinate space. Corners measured on our unit: TL (27,38), TR (318,59),
BL (23,419), BR (318,412).

**Known wrong for our unit:** the generic CST816S datasheet's `0xFE`
"DisAutoSleep" register **is not in this part's register map**. Ignore any
DisAutoSleep advice found online. This is the one confirmed deviation from
the generic docs, and it is the reason to distrust the rest of them
(auto-sleep timeout `0xF9`, gesture-wake mask `0xEC`, the software-sleep
command `0xA5 = 0x03`) until each is probed on hardware.

## It shares the panel's power rail

`DCDC1 3.3 V → VCC3V3 → DSI_PWR_EN (TCA9554) → VCI → CO5300 **and**
CST820`. One supply domain, and the consequences run through the whole
design:

- Screen sleep cuts that rail, so **the digitiser is unpowered while the
  screen is off**. PWR wakes the device; touch cannot.
- Arduino-era, same cause: `Arduino_CO5300::displayOff()` sends DISPOFF then
  SLPIN, and sleeping the panel controller took the digitiser with it —
  observed as the raw touch registers freezing entirely, with nothing able
  to wake the device.
- SDA, SCL and TP_INT stay pulled up regardless (shared with the PMU or
  pulled up off-chip), so they back-feed the dead digitiser through its
  protection diodes at whatever the pull-ups allow — order 0.4 mA.

### NACKs are not proof the rail is down

The README calls `touchdump` answering `DUMP read failed` "the check that
the rail is down". **That check is contaminated**, and it is worth knowing
why before trusting it: `cutPower()` writes `0x00` to the expander output
register, which drops **all three** bits at once — DSI_PWR_EN *and* TP_RST
*and* LCD_RST. A chip held in reset NACKs just as convincingly as a chip
with no supply. So a NACK proves "the expander bits are low", nothing
finer; it cannot distinguish rail-down from reset-held, and any experiment
that needs that distinction has to drive the bits independently.

## INT → GPIO21, reserved and unconfigured

TP_INT reaches **GPIO21** through a 10 kΩ pull-up to VCC3V3 (schematic, and
independently the vendor BSP's `BSP_LCD_TOUCH_INT (GPIO_NUM_21)`). Nothing
in `main/` configures it today — it is free, and it is the wake path task
#22 (wake-on-touch) would use:

```c
gpio_wakeup_enable(GPIO_NUM_21, GPIO_INTR_LOW_LEVEL);
esp_sleep_enable_gpio_wakeup();
```

Idle-high with an active-low pulse is the family norm, so level-low is the
expected trigger — **polarity unverified on our unit, bench-check first.**
Arm it once (wakeup sources are a persistent bitmap, not a per-sleep arm),
and note that if `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP` is ever
turned on, plain `esp_sleep_enable_gpio_wakeup()` stops working. It is off
today.

## Low-power figures — datasheet only, UNCONFIRMED on our unit

Every number below comes from **CST816-labeled** datasheets, cross-checked
across independent excerpts because the PDFs themselves would not extract.
Our part is a CST820 wearing a CST816-shaped driver, and it has already been
caught deviating once (`0xFE`). Treat these as starting estimates:

| Mode | Typical | Behaviour |
|---|---|---|
| Dynamic (finger down / just lifted) | < 2.5 mA | Full-rate scan |
| Monitor (brief transition) | not published | Lower-rate scan, promotes back on contact |
| **Standby** (auto after ~2 s inactivity) | **< 10 µA** | Low-frequency scan; a touch wakes it to Dynamic **and pulses INT** |
| Sleep (software-forced) | < 1–5 µA | Wake needs a hardware reset or a pre-armed gesture, not "any touch" |

One family behaviour worth flagging: several CST8xx parts **power off their
own I2C interface** in Standby/Sleep and only answer again once woken. You
cannot poll your way out of low power on this family — you have to interrupt
your way out, which is an argument *for* the GPIO21 path, and confirms that
the awake-path I2C polling this firmware does today has no role once the
chip is genuinely asleep.

Combined with the CO5300's ~110–135 µA SLPIN figure
([co5300-display.md](co5300-display.md)), "rail up + SLPIN + touch standby"
lands on the order of 150 µA — but the module's own upstream regulator
overhead is unknown and is the number that decides whether wake-on-touch is
free. Bench soak required.
