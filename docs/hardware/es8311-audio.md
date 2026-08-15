# ES8311 + NS4150B — the audio path

**Status: brought up and measured on this board 2026-08-16** (backup unit, USB
powered). The beeper lives in `main/audio.cpp`; `beep on|off` and `micdump
<ms>` drive it from the console. Claims below are marked *measured* or
*datasheet only*.

## Parts

- **ES8311** codec at I2C `0x18` — 7-bit; `esp_codec_dev` wants the 8-bit form
  and calls it `ES8311_CODEC_DEFAULT_ADDR` (`0x30`).
- **NS4150B** mono filterless Class-D power amplifier, 3–5.25 V supply, run
  off VCC3V3 here. ~4 mA quiescent when enabled with no signal (datasheet).
- **Onboard 8 Ω / 1 W speaker** and an **onboard analog microphone**,
  digitised by the same ES8311. No external hardware needed — the microphone
  hears the speaker loudly, which is what makes the beep machine-verifiable
  (`tools/beeptest.py`).

## Pins

| Signal | GPIO |
|---|---|
| I2S MCLK | **16** |
| I2S BCLK (SCLK) | **9** |
| I2S WS (LRCK) | **45** |
| I2S DOUT (ESP → codec DAC) | **8** |
| I2S DIN (codec ADC → ESP, mic) | **10** |
| PA_CTRL (amp enable) | **46** |

**PA_CTRL is a direct GPIO — not routed through the TCA9554** like the
display's power rail. Do not go looking for it on the expander.

No conflicts: the panel owns 4/5/6/7/11/12, I2C owns 14/15, BOOT is 0, touch
INT is 21.

## The amplifier takes ~175 ms to wake (measured)

The single fact that costs the most time if you do not know it. `PA_CTRL` high
does not mean audio: for roughly the first 175 ms after the enable edge the
NS4150B passes **nothing**. A 200 ms burst written straight after the enable
is heard as its last ~25 ms, which looks exactly like a codec or DMA fault and
is neither.

Measured three ways: a 200 ms burst was audible for 23 ms; an 800 ms
amplitude-staircase burst was audible for its last 640 ms (the first two steps
missing, later steps intact — so the loss is at the *start*, not the end); and
enabling the amplifier 250 ms ahead of the same 200 ms burst restored the full
210 ms of tone. `main/audio.cpp` therefore raises `PA_CTRL` `PA_SETTLE_MS`
(200 ms) before the first sample.

**That wait is paid once per session, not once per beep.** `PA_CTRL` stays high
across the gaps for as long as there is beeping to do, and only goes low when
the beeping stops. It costs the amplifier's ~4 mA quiescent draw for the length
of a session (minutes, at most).

Be precise about what that buys, because it is easy to overclaim. **The first
beep of a session is not faster**: `amplifier(true)` is called from inside
`burst()`, so burst 1 still waits out the mute window before its first sample.
**Bursts were already full-length before the change** — pre-enabling per burst
was Task 1's whole fix. What changes is that beeps 2..N skip the mute window
entirely, so the cadence tightens by exactly one `PA_SETTLE_MS`, and the
amplifier is no longer switched on and off between every burst:

| `PA_CTRL` | beep spacing | tone per burst |
|---|---|---|
| raised per burst | 1.92 s | 210 ms |
| held for the session | 1.72 s | 210 ms |

Mic-verified both ways. The 200 ms of the difference is the settle wait, no
longer spent between beeps.

The amplifier still drops on `beep off` even while a `micdump` holds the
session open, which is what keeps "no tone with the amplifier off" available as
a control the microphone can be shown.

**Polarity is active-high** (measured — the tone is only heard while GPIO46 is
high; matches the vendor BSP's `pa_reverted = false`). This settles the
"UNVERIFIED, check with a meter" warning that stood here before.

## Strapping pins (measured)

GPIO45 (WS, VDD_SPI voltage select) and GPIO46 (PA_CTRL, boot mode) are both
strapping pins, and the i2s driver **does not hand its pads back** on
`i2s_del_channel` — `esp_gpio_revoke` only frees the reservation bitmap, the
pad stays routed and driven. `sessionDown()` puts MCLK/BCLK/WS/DOUT/DIN back
to `GPIO_MODE_DISABLE` (hi-Z, no pulls, so the board's own resistors decide)
and leaves PA_CTRL driven **low** — which is both "amplifier off" and the safe
strap level, and beats hi-Z for an amplifier enable pin.

Six resets — three with the chain torn down, three in the middle of an active
beep session (PA high, WS clocking) — all came up
`boot:0x2b (SPI_FAST_FLASH_BOOT)`. No strapping regression either way.

## Driver stack: `esp_codec_dev`, not `es8311`

**The earlier recommendation of the small `espressif/es8311` component is
wrong for this repo and was dropped.** That component is built against the
legacy `driver/i2c.h` API (`es8311_create(i2c_port_t, addr)`), and this
firmware runs the modern `i2c_master` bus shared with the PMU, expander and
touch (`main/i2c.cpp`); the two drivers cannot both own I2C_NUM_0.
`espressif/esp_codec_dev` (1.6.2) takes an `i2c_master_bus_handle_t` straight
(`audio_codec_i2c_cfg_t::bus_handle`), which is why `i2cBus()` exists.

Configuration that works, with the reasons:

- `es8311_codec_cfg_t::pa_pin = -1` — the component would otherwise hold the
  amplifier on for the whole session; `audio.cpp` gates GPIO46 per burst.
- `use_mclk = true`. MCLK is wired to GPIO16, so the MCLK-less/BCLK-derived
  mode is not needed. `mclk_multiple` 256×Fs (16 kHz → 4.096 MHz).
- `codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH`, `dev_type =
  ESP_CODEC_DEV_TYPE_IN_OUT` — one open drives both DAC and ADC.
- **Channel count must be even.** `esp_codec_dev`'s I2S data interface rejects
  an odd `channel`, so a mono codec is driven as 2×16-bit slots at 16 kHz with
  `channel_mask = 0x03`. Playback duplicates the sample into both slots.
- `no_dac_ref = true` is meant to leave the right slot of a recording empty
  instead of filling it with the DAC's output. **Measured: it does not — both
  recorded slots are bit-identical microphone data.** No harm here (a capture
  of the beep therefore cannot be a DAC-loopback artifact either way), but do
  not rely on the right slot being a DAC reference.
- Data is written and read with `i2s_channel_write` / `i2s_channel_read`
  directly rather than `esp_codec_dev_write/read`: the beeping task and a
  `micdump` on the console task then touch two independent channel handles,
  which is what allows recording *while* beeping.

`esp_codec_dev_open` reconfigures both channels to match the format it is
given (it calls `i2s_channel_reconfig_std_*`), and `esp_codec_dev_close`
**disables both i2s channels** on the way out — disabling them again before
`i2s_del_channel` only produces `i2s_channel_disable(): the channel has not
been enabled yet` in the log.

## Power (measured)

- While a session is up the i2s driver holds **two `i2s_driver`
  `APB_FREQ_MAX`** PM locks (one per channel). `i2s_del_channel` deletes them:
  after `beep off`, `power locks` lists only `usb` again. Nothing lingers.
  These locks **suspend automatic light sleep for the entire session** — the
  chip stays at or above `PM_MODE_APB_MAX` while audio is running, and light
  sleep resumes only after the session ends (bounded by the 10-minute finder
  timeout).
- The DMA buffers cost about **12 KB of internal heap** for the session
  (internal free 36.5 KB idle → 24.0 KB with the chain up; 35.7 KB → 23.1 KB
  with Matter up and advertising, on an uncommissioned board).
- ES8311 normal-operation current ~8 mA, power-down ~0 µA (datasheet). The
  codec is opened once per session, not per beep: re-running the init sequence
  every two seconds buys settle risk for nothing.

## Odds and ends (measured)

- A write returns when the bytes are in the DMA buffers, not when they have
  been clocked out: 200 ms of audio takes ~124 ms of wall time to write, so
  ~76 ms is still queued when the last write returns. Hence `DMA_DRAIN_MS`.
- Bringing the codec up produces a short transient the microphone sees even
  with the amplifier off (~11000 rms for one 20 ms window, gone within 40 ms).
  Harmless, but it is why a capture's first windows are not the noise floor.
- Microphone gain: **0 dB is the right setting for recording our own beep.**
  At 24 dB the capture clips flat and the analogue front end needs tens of
  milliseconds to recover, which reads as a beep that stopped early.
- 2 kHz at 16 kHz is exactly 8 samples per period, so a 20 ms tone chunk holds
  40 whole periods and can be written back to back without a step.
