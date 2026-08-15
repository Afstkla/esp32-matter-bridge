# ES8311 + NS4150B — the audio path

**Status: researched 2026-08-15, desk research only. Nothing here has been
flashed or measured on this board. Bring-up is in progress under task #24
(Find-My-Genie beeper).** Everything below is schematic-dump, vendor-wiki or
datasheet sourced; treat it as a wiring guide, not as verified behaviour.

## Parts

- **ES8311** codec at I2C `0x18` (the Waveshare wiki confirms the codec but
  not its address; the address is from our own I2C scan).
- **NS4150B** mono filterless Class-D power amplifier, 3–5.25 V supply, run
  off VCC3V3 here. ~4 mA quiescent when enabled with no signal.
- **Onboard 8 Ω / 1 W speaker** (schematic label "12×10 mm 8 Ω 1 W", and the
  Waveshare wiki lists an onboard speaker) and an **onboard analog
  microphone**, digitised by the same ES8311. No external hardware needed.

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
INT is 21. Nothing in `main/` touches 8/9/10/16/45/46, and 0x18 is free on
the bus.

## Gotchas

- **GPIO45 and GPIO46 are ESP32-S3 strapping pins** (VDD_SPI voltage select
  and boot-mode/ROM-log select). The board boots fine as wired, but nothing
  may drive either pin *before* `app_main()` — configure them after boot,
  the same way everything else here does.
- **MCLK is wired and mandatory.** GPIO16 carries a real MCLK to the codec,
  so do not reach for the ES8311's MCLK-less / BCLK-derived clocking mode.
  Datasheet limits: MCLK max 49.2 MHz, 40–60 % duty; `mclk_multiple` 256×Fs
  is the normal choice.
- **NS4150B enable polarity is UNVERIFIED.** Do not assume active-high —
  check GPIO46 with a meter or logic probe during first bring-up before
  wiring any "amp off between beeps" logic around it. Getting it wrong means
  either always-on (an extra ~4 mA and a pop) or always-silent, not damage.
- ES8311 normal-operation current is ~8 mA typical; power-down is ~0 µA.
  Gate the **amp** per beep, not the codec — re-running the codec init
  sequence for every beep buys pop and settle risk for nothing.

## Recommended stack

`espressif/es8311` (an I2C-register-sequence-only driver; the caller owns
the I2S port) plus the IDF `i2s_std` driver. The registry flags `es8311` as
deprecated in favour of `espressif/esp_codec_dev`, which is a heavier
multi-codec device abstraction — for one codec with one purpose it buys
nothing, and this repo hand-rolls every other peripheral anyway. Revisit if
audio ever has to share I2S with a second consumer (mic capture, say).
