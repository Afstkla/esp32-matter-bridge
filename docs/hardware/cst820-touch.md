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

## The register map, read off our own unit

`touchreg` dumps all 256 addresses (`touchreg <reg> [<value>]` reads or
writes one). Idle, untouched, with the rail up, our unit reads:

```
00: 00 00 00 00 00 00 00 00 00 FF FF FF FF FF FF FF
10: FF FF FF FF 00 00 00 00 00 00 00 00 00 00 00 00
20: 00 00 00 00 00 00 00 00 00 FF FF FF FF FF FF FF   <- 0x00..0x1F, mirrored
30: FF FF FF FF 00 00 00 00 00 00 00 00 00 00 00 00
40..9F: all 00
A0: 00 00 00 00 00 00 00 B7 41 02 FF 00 00 00 00 00
B0: 01 59 01 5A 00 00 00 00 00 00 00 00 00 00 00 00
C0: 00 00 00 00 00 00 00 00 00 00 00 00 42 43 44 45
D0: 46 56 55 10 40 41 50 51 52 53 54 17 00 00 00 00
E0: 00 00 00 00 00 00 00 00 00 00 07 00 01 01 00 00
F0: 00 00 00 00 00 00 00 00 00 00 70 00 00 17 01 00
```

Every address ACKs — the part answers its whole 8-bit space, so a NAK is
never "no such register", it is always "the chip is not listening". That
distinction is what the old `0xFE` story got wrong (below). `0x00..0x1F`
mirrors into `0x20..0x3F`, so the report area is 32 bytes wide and decoded on
the low five bits.

`0xA7 = 0xB7` chip ID, `0xA8 = 0x41` project ID, `0xA9 = 0x02` firmware
version. `0xFA` **always reads back `0x70`** whatever is written to it — the
write lands (touch reporting depends on it) but the readback is not the value
you wrote, so `0xFA` cannot be used to probe which interrupt modes exist.
Writing `0xFA = 0x80` (the generic map's "EnTest", which is supposed to make
INT emit periodic pulses) produced **no INT activity at all** over 3 s at 1 ms
sampling: there is no self-test pulse on this part, and therefore no way to
exercise the INT line without a finger.

## Standby is reachable, and it is OFF by default

**Correction — the old "`0xFE` is not in this part's register map" claim is
wrong.** `0xFE` exists, reads `0x01` out of reset, and is exactly the generic
map's **DisAutoSleep**: non-zero means *do not* auto-enter low power. Our unit
therefore ships with **auto-standby disabled**, which is why nothing here had
ever seen the chip go quiet. What the earlier investigation actually saw was
the register working:

```
touchreg 0xFE        -> FE=01          (default: auto-sleep disabled)
touchreg 0xFE 0x00   -> write ok       (auto-sleep enabled)
touchreg 0xFE        -> ESP_ERR_INVALID_STATE   (NAK — the chip is in standby)
touchreg 0xA7        -> ESP_ERR_INVALID_STATE
tpint                -> level=1        (INT idles high all the way through)
```

The write succeeds and the chip drops into standby within one transaction;
its I2C interface goes with it, so the *next* read NAKs. Read that NAK as
"absent register" and you get the old conclusion. This also confirms on our
own part the family behaviour the datasheets describe: **you cannot poll your
way out of low power, only interrupt your way out.**

Recovery is real but not tidy. Sometimes a couple of failed transactions rouse
it and reads resume (`0xA7 = 0xB7` about 2 s later); sometimes it stays deaf
while the ui task's 20 ms `pollTouch()` keeps hammering it. **A TP_RST pulse
always fixes it** — `sleep` then `wake` (which runs `releaseResets()`) restores
both the chip and `0xFE = 0x01`. Registers are volatile; nothing persists.

### How the firmware enters and leaves it (tier 1)

`panelDoze()` writes `0xFE = 0x00` after the panel's SLPIN, and `panelRouse()`
brings the part back with a **TP_RESET-only pulse** — expander output register
`0x01`: `0x07 → 0x03 → 0x07`, 20 ms each way, then the usual `touchInit()`
retry loop. Clearing EXIO2 alone leaves DSI_PWR_EN and LCD_RESET high, so the
digitiser restarts without the module's 261 ms cold start; `releaseResets()`
(which drops all three) is the tier-2 path and stays that way.

**Measured, 10+ cycles on our unit: it works.** `CST816 chip id 0xB7 after
20 ms` on every rouse — one 20 ms retry, against `after 0 ms` for the rail-cut
path, so a part coming out of standby by reset is marginally slower to answer
than one coming up from cold. The whole rouse is **220–221 ms** at settle 120:
139 ms of panel plus ~81 ms of digitiser (40 ms pulse, 20 ms retry, 20 ms
post-`0xFA` settle). No failed revival in any cycle.

Two rules follow from standby killing the I2C interface, and the firmware obeys
both: `panelTouch()` is fenced off on `panelDozing()` so nothing polls a part
that cannot answer, and a `touchInit()` that fails after the reset pulse is
treated as a failed rouse — the rail drops and the caller cold-starts the
module. A lit screen with a deaf digitiser is the worse outcome, since touch is
the whole point of the tier.

`0xA5 = 0x03` (the generic software-sleep command) is **still unprobed**: every
attempt to write it landed while the chip was already in standby and NAKed.

**Still unverified, and it is the load-bearing one:** whether a finger pulls
INT low *out of* standby. Nothing on this part can fake a touch — no test
pulse mode (above), and driving GPIO21 from the SoC proves the SoC side only.
This is a ten-second human check: `touchreg 0xFE 0x00`, wait, tap the glass,
`tpint scope 3000` should show `falls>0` and `touchreg 0xA7` should answer.

The remaining generic-map claims (auto-sleep timeout `0xF9` — reads `0x00`
here, gesture-wake mask `0xEC` — reads `0x01`) are still unprobed. Given
`0xFE` turned out to be *right* rather than absent, the lesson is the reverse
of the one this file used to draw: distrust the earlier refutations as much as
the datasheets, and re-probe with the NAK-means-asleep reading in mind.

## It shares the panel's power rail

`DCDC1 3.3 V → VCC3V3 → DSI_PWR_EN (TCA9554) → VCI → CO5300 **and**
CST820`. One supply domain, and the consequences run through the whole
design:

- Tier-2 screen sleep cuts that rail, so **the digitiser is unpowered** and PWR
  is the only wake. Tier 1 (the first minute) leaves the rail up precisely so
  that it is not.
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

**Idle level measured: high (1).** Rail up with the digitiser alive, rail cut
with it unpowered, and in standby — GPIO21 reads 1 in all three, so the 10 kΩ
pull-up is on the always-on side of `DSI_PWR_EN` and **level-low is the right
trigger**. It never asserts on its own: 3 s at 1 ms sampling with the chip
awake and configured shows `low=0 falls=0`.

**Nothing had configured the pad, and that reads exactly like a stuck line.**
An unconfigured ESP32-S3 GPIO has its input buffer off and `gpio_get_level()`
returns 0 regardless of the wire. The first measurement here said "idle low"
for that reason alone. Call `gpio_config()` with `GPIO_MODE_INPUT` before
believing any reading — `powerBegin()` now does.

Arm it once (wakeup sources are a persistent bitmap, not a per-sleep arm).
Two config caveats:

- `CONFIG_PM_SLP_DISABLE_GPIO` **is on** — not by choice; the light-sleep GPIO
  reset workaround `select`s it. It isolates every pad on the way into
  automatic sleep, which would make GPIO21 deaf. `gpio_wakeup_enable()` is
  what saves this: it calls `gpio_hal_sleep_sel_dis()` on the pin under that
  same Kconfig. Arming by hand through `rtc_gpio_wakeup_enable()` would skip
  it and the pin would sleep through every touch.
- if `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP` is ever turned on, plain
  `esp_sleep_enable_gpio_wakeup()` stops working. It is off today, and GPIO21
  is an RTC IO (the S3's RTC range is 0–21), so the
  `..._on_hp_periph_powerdown()` variant would be available if it ever matters.

### `tpint drive 0` simulates a touch wake — and cannot simulate a stuck line

Two bench facts, both learned the hard way and both worth having before
designing a test around this pin.

**Driving the line low while tier 1 is armed exercises the entire wake path**
without a finger: the ISR latches (`tpint` reports `fired=1` in the same
reply), the ui tick rouses, and `WAKE touch` follows. Measured end to end,
three runs: **390–470 ms** from the drive to `WAKE touch`, of which 221 ms is
the rouse and the rest is the polled latch waiting for the next 250 ms tick.
Everything downstream of "INT goes low" is therefore machine-verifiable; the
only thing a finger is still needed for is whether the CST820 asserts INT out
of standby at all.

**The refused-arm guard cannot be provoked from the SoC side.**
`powerArmTouchWake()` opens with `openTpInt()`, whose `gpio_config()` resets the
pad to plain input and so *releases the open-drain drive* before the level check
reads it — `tpint drive 0` then `sleep` arms normally, with `level=1`. (This is
the same effect that invalidated Task 1's first disarmed-held-low control.) The
guard still protects against the case it was written for, a digitiser holding
INT low with its own output, because nothing on the SoC side can release that.
It just cannot be rehearsed on the bench.

`tpint` is the bench instrument for all of this: level and armed state with no
argument, `arm`/`disarm`, `drive <0|1|z>` to pull the line open-drain from the
SoC side (open-drain, so it can never fight the digitiser), `scope <ms>` to
sample the line every millisecond, and `watch <ms>` to run a battery-simulated
window and count what actually woke the chip. `watch` keeps its last result so
`tpint` can print it afterwards — light sleep stutters USB-CDC and the line the
window itself emits is the one most likely to be swallowed.

### The GPIO wake source *is* honoured by automatic light sleep

The open question the desk research flagged — IDF documents GPIO wakeup only
against manual `esp_light_sleep_start()`, never against the `esp_pm` tickless
path. Answered on the bench, screen asleep, `usbsim` on, BLE shut down so the
chip can actually sleep:

| Run | armed | INT | result |
|---|---|---|---|
| control | yes | idle high | **199 light sleeps, 0 rejects**, every sample woke on TIMER (`mask=0x10`) |
| test | yes | held low | **0 further sleeps, 4442 rejects in 8 s** |

The only variable is the line level. A level-triggered source already sitting
at its trigger level does not wake the chip repeatedly — it makes the hardware
*refuse* to sleep, and IDF builds that reject mask from the very same bitmap:
`reject_triggers = s_config.wakeup_triggers & RTC_SLEEP_REJECT_MASK`, and
`RTC_GPIO_TRIG_EN` is in that mask. The mask that compiles for this chip is
the one in `esp_hw_support/port/esp32s3/include/soc/rtc.h:677,694` — *not* the
copy in `esp_private/esp_pmu.h`, which is gated `#if SOC_PMU_SUPPORTED` and
the S3 does not define that. The automatic
path adds its own timer source on top and calls the same
`esp_light_sleep_start()` (`pm_impl.c`'s `vApplicationSleep`), clearing
nothing. So the GPIO source is armed, live and evaluated on the automatic path.

### The wake source is not the whole wake: the line has to be latched

Waking the chip only *shortens a light sleep*. The ui task is still parked in
its 250 ms `vTaskDelay` and learns nothing, and the INT assertion is a pulse —
the generic map puts `0xED` (IrqPluseWidth) at 1 ms by default, which no 250 ms
poll will ever see. So `powerArmTouchWake()` arms the wake source **and** hangs
a GPIO ISR on the same pad; the handler sets a flag the ui tick reads.

That handler has to mask itself, because the trigger is a level and a level ISR
re-enters for as long as the line is down: `gpio_isr_loop()` clears the pending
status on entry only for pins in `isr_clr_on_entry_mask`, and
`gpio_set_intr_type()` puts **only edge types** in that mask, so a level pin's
status is sticky across the handler.

`gpio_intr_disable()` is the right mask, and it is worth knowing exactly what it
does. `gpio.c:192` calls `gpio_hal_intr_disable()`, which is two things
(`components/hal/gpio_hal.c:24`): `gpio_ll_intr_disable()` — `pin[n].int_ena = 0`
— **and** a clear of the pending status bit, which is the second half of why the
self-mask works at all. What it does *not* touch is `int_type` (the low-level
trigger) or `wakeup_enable`: neither is written anywhere on that path, so the
light-sleep wake source stays armed through the mask and only an explicit
disarm takes it down. Nothing in the API contract says so — the source does
(`esp_driver_gpio/src/gpio.c`, `hal/gpio_hal.c`, `hal/esp32s3/include/hal/gpio_ll.h`
in the pinned IDF v5.5.5).

The same level semantics are why arming is skipped when the line is already low
(`TP_INT already low, leaving touch wake disarmed`): the handler would fire at
once, the screen would wake, idle out and re-arm in a loop, and the hardware
would refuse every light sleep in between (the 4442 rejects above). INT idles
high powered, in standby and unpowered alike, so a low line at arm time is a
fault, and losing touch wake for that one cycle is the safe reading of it — the
tier-1 window still expires into the rail cut. **The refusal only degrades
cleanly if the latch is cleared with it**, which is the first thing
`powerArmTouchWake()` does on every transition: a latch surviving from the last
touch wake fires ~250 ms into the next tier-1 entry, and the "skip touch wake
this cycle" fault becomes the wake loop the check was written to prevent.

Because a 1 ms pulse has to survive a CPU-power-gated light sleep to reach that
handler at all (`CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP` is set — though on our
unit it does not take: every boot logs `sleep: sleep_cpu_configure(236): Failed
to enable CPU power down during light sleep`, so the CPU is *not* in fact power
gated and the risk this insures against is not currently armed),
`powerTouchWoke()` also reads `esp_sleep_get_wakeup_causes()` for
`BIT(ESP_SLEEP_WAKEUP_GPIO)`. GPIO21 is the only GPIO wake source in this
firmware, so that bit can only mean this pin. Two independent readings: without
the second, a latch that does not survive the resume would look exactly like a
finger that never pulled INT low, and the bench would blame the digitiser.

**What is still not proven is the last hop**: a low pulse *arriving while the
chip is asleep*. Nothing on this board can produce one on demand — the CST820
has no test-pulse mode, and the S3's LEDC has no RC_FAST clock option
(`SOC_LEDC_SUPPORT_RC_FAST_CLOCK` is absent for esp32s3), so no peripheral can
drive the pad through a light sleep. That hop is ordinary level-detector
hardware and is what the IDF light_sleep example demonstrates, but it is a
finger, not an argument. **Human check: arm, sleep the screen, tap the glass,
confirm the wake.**

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
