# The board — Waveshare ESP32-S3-Touch-AMOLED-1.8 (V2)

Device-level facts. Per-chip detail lives in the sibling files.

**This is a V2 board.** Waveshare silently revised the product: same name,
same pinout, same 368×448 panel, different main chips.

| | V1 | **V2 (ours)** |
|---|---|---|
| Display | SH8601 | **CO5300** |
| Touch | FT3168 @ 0x38 | **CST820 @ 0x15** (CST816 family) |

Verified on our unit by I2C scan (0x15 present, 0x38 absent) and by
`strings` over the factory flash dump: 31 hits for `co5300`, 14 for
`cst816`, zero for `sh8601`/`ft3168`. Most community code and every
published ESPHome config for "the 1.8" targets V1 and fails at touch init.

## Silicon and memory

- ESP32-S3, QFN56 package, **rev v0.2**
- **8 MB PSRAM, OPI** — the framebuffer lives here (322 KB will not fit in
  internal RAM beside Matter)
- **16 MB flash, QIO**
- 2.4 GHz radio only; a 5 GHz-only SSID fails silently

## USB-Serial-JTAG

USB is the chip's own built-in USB-Serial-JTAG peripheral, not an external
bridge. It enumerates as "USB JTAG/serial debug unit" in ROM and in the
application alike, and keeps the same port across resets. It works through
a USB-C monitor hub, flashing included. Only one reader may hold the port
at a time.

**The known wedge.** Long light-sleep windows wedge the port: with
`power usbsim on` (or on battery generally) the device light-sleeps, USB-CDC
stutters and then dies, and **only a physical replug recovers it** — no
software reset, no `esptool` reconnect. The firmware is unaffected the whole
time: screen and touch keep working, and the network console keeps
answering. Rules that follow:

- Keep USB serial windows during sleep experiments **≤ 30 s**; drive
  anything longer over the network console (port 5323), which survives.
- If the port disappears and does not come back within ~10 s, stop and ask
  the human to replug. Do not keep retrying.
- This is why `powerBegin()` holds an `ESP_PM_NO_LIGHT_SLEEP` lock while
  VBUS is present: on USB the console must stay alive.

## Two things silently pin the device awake

Neither is visible in `power`, which happily prints `light_sleep=1` while the
chip has never slept once. `power locks` is the honest readout — the
`Sleep stats: light_sleep_counts` line at the bottom is the number that
settles it.

- **CHIPoBLE holds `NO_LIGHT_SLEEP` for the whole life of an uncommissioned
  device.** The BT controller takes a lock named `btLS` when it is enabled and
  only releases it when CHIP deinits BLE — which happens on *commissioning
  completion*, not when the pairing window expires
  (`BLEManagerImpl::DriveBLEState` deinits only once the CHIPoBLE service mode
  leaves `Enabled`). So a factory-fresh or decommissioned Genie **never light
  sleeps at all**, cable or no cable, and any bench measurement of sleep on an
  uncommissioned board reads 100% awake. To measure anyway, schedule
  `BLEMgr().Shutdown()` onto the CHIP task (`ScheduleWork`); the log answers
  `BLE deinit successful and memory reclaimed` and the lock disappears.
- **A board with no battery used to pin itself awake too.** `powerPoll()` bailed
  on `pmu.millivolts == 0` *before* applying the USB lock, so a batteryless
  board (`PMU batt=0mV`) held `usb` forever and `power usbsim on` did nothing.
  Fixed — the lock is applied on `pmu.present` alone, which is all VBUS needs.

**Sticky USB download mode** (Arduino-era finding, hardware behaviour, still
true): the S3 stays in download mode across a `USB_UART_CHIP_RESET`, so once
in, no number of resets boots the app — `boot:0x23 (DOWNLOAD(USB/UART0))` in
the ROM banner is the tell. Only a real power-off clears it, and **with a
battery attached, unplugging USB is not a power-off** — hold PWR ~10 s to
make the AXP2101 cut the rails. In practice this is rarely needed:
`esptool --after watchdog-reset` leaves download mode properly.

## Buttons

Two buttons on the edge. **BOOT is the top one, PWR is the bottom one**, and
they are read completely differently.

| Button | Position | How it is read |
|---|---|---|
| BOOT | **top** | GPIO0, active low, plain `gpio_get_level`, edge-on-release |
| PWR | **bottom** | AXP2101 PEK — not a GPIO. Enable the short-press IRQ (INTEN2 bit 3), poll INTSTS2, clear the status |

**Long-press PWR is the PMU's own power-off** and is deliberately left
alone: only the short-press IRQ is enabled, so a long hold still cuts the
rails as the hardware intends. There is **no reset button** on this board.

Anything reading the PMU needs the I2C bus up first — `keysInit()` runs
after the bus exists.

## Strapping pins

GPIO45 (VDD_SPI voltage select) and GPIO46 (boot mode / ROM log select) are
**ESP32-S3 strapping pins**, and both are wired to the audio subsystem
(WS and PA_CTRL). The board boots fine as wired, but nothing may drive
either pin *before* `app_main()` — configure them the normal way, after
boot, like everything else here does.

## I2C bus — SDA = 15, SCL = 14, 400 kHz

One `i2c_master` bus shared by everything:

| Addr | Chip | File |
|---|---|---|
| 0x15 | CST820 touch (V2; V1 has FT3168 @ 0x38) | [cst820-touch.md](cst820-touch.md) |
| 0x18 | ES8311 audio codec | [es8311-audio.md](es8311-audio.md) |
| 0x20 | TCA9554 IO expander | [tca9554-expander.md](tca9554-expander.md) |
| 0x34 | AXP2101 PMU | [axp2101-pmu.md](axp2101-pmu.md) |
| 0x51 | PCF85063 RTC | [pcf85063-rtc.md](pcf85063-rtc.md) |
| 0x6B | QMI8658 IMU | [qmi8658-imu.md](qmi8658-imu.md) |

## Power rail topology

```
AXP2101 DCDC1 (3.3 V)  →  VCC3V3  →  DSI_PWR_EN (TCA9554 EXIO)  →  VCI
                                                                    ├─ CO5300 AMOLED
                                                                    └─ CST820 touch
```

**The panel and the digitiser share one supply domain.** That single fact
drives most of this board's power behaviour, and it is why screen sleep is
two tiers rather than one: for the first minute the rail stays up with the
panel in SLPIN and the digitiser in standby, so a finger still wakes the
device (~200 ms back); after it the rail goes down for the larger half of
the shelf drain, and the PWR key is the only wake left (~261 ms cold start).
See [co5300-display.md](co5300-display.md) and
[cst820-touch.md](cst820-touch.md).

`DCDC1 = VCC3V3` was established while chasing the shelf drain; the firmware
programs no regulator, so every other rail sits at the PMU's power-on
default.

## Pin usage

| Signal | GPIO | Notes |
|---|---|---|
| LCD CS | 12 | QSPI |
| LCD SCK | 11 | QSPI, **40 MHz** (80 MHz underflows the PSRAM DMA) |
| LCD D0..D3 | 4, 5, 6, 7 | QSPI |
| I2C SDA | 15 | shared bus |
| I2C SCL | 14 | shared bus |
| BOOT key | 0 | active low, internal pull-up |
| **TP_INT** | **21** | CST820 interrupt, 10 kΩ pull-up to VCC3V3 — **measured idle-high** with the rail both up and cut, so the pull-up is upstream of `DSI_PWR_EN` and level-low is the wake trigger. `powerBegin()` configures it as an input and installs the GPIO ISR service; the ui task arms it (wake source **and** a self-masking level ISR) for the length of tier 1, and the `tpint` console command drives the same plumbing by hand. See [cst820-touch.md](cst820-touch.md) |
| I2S MCLK | 16 | audio, wired and mandatory |
| I2S BCLK | 9 | audio |
| I2S WS | 45 | audio, **strapping pin** |
| I2S DOUT (ESP→codec) | 8 | audio |
| I2S DIN (mic→ESP) | 10 | audio |
| PA_CTRL | 46 | NS4150B enable, **direct GPIO, not via the expander**, **strapping pin** |

LCD reset, touch reset and the panel rail reach no GPIO at all — they hang
off the TCA9554. Audio pins are desk-research-sourced (schematic dump +
vendor BSP) and not yet exercised; everything else is in use today.

## Not in this repo

The private `../NOTES.md` (outside the repo, never committed) holds board
history and credentials. MAC addresses, LAN addresses, the Wi-Fi SSID and
the pairing code live there and **must never appear in a committed file** —
run the privacy grep before every commit.
