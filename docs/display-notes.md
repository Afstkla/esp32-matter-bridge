# Display pipeline: what we know, what bit us

The CO5300 flush path lore. Companion to docs/matter-notes.md (Matter serve
paths) and components/esp_lcd/PATCH.md (the vendored driver fix). Everything
here was learned the hard way; verify file:line cites against the pinned
IDF (5.5.5) before trusting them in a different version.

## Frame geometry (grounds every DMA argument)

368×448 @ RGB565 → 736 bytes/row, 329,728 bytes/frame. esp_lcd's SPI chunk
cap is 32,768 B → a frame is **11 queued transactions of ~44.5 rows each**.
GDMA descriptors on ESP32-S3 cap at 4095 B (12-bit length) → **~5.6 rows
per descriptor**. Two visual signatures fall out of this arithmetic:

- **~44-row garbled band** = one whole chunk went bad (the known,
  counted-and-re-flushed underflow signature; see PATCH.md).
- **A few-pixel-high band** = descriptor-granularity (~4 KB) damage.

## How the flush works on the wire

`esp_lcd_co5300` sends CASET+RASET once, then ONE RAMWR that spans all 11
chunks under a single CS assertion (`SPI_TRANS_CS_KEEP_ACTIVE` on all but
the last). Consequences:

- The panel's GRAM write pointer auto-increments across chunk boundaries;
  a *skipped* chunk would leave STALE pixels (or a torn/shifted frame if
  the pointer desyncs) — **never a clean black band**.
- A clean black band means zeros were TRANSMITTED, on schedule, as normal
  pixel data. The panel did what it was told.

## What the underflow detector actually detects

`spi_trans_dma_error_check()` (IDF `spi_master.c:951-969`) runs once per
queued chunk and reads `dma_int_raw.outfifo_empty_err` — a FIFO
**starvation/timing** flag. The S3 TRM (`spi_struct.h:266`): on underrun
the SPI **stops in master mode** (slave mode sends zeros — not us). So:

- A real underrun stalls the clock and gets flagged (or is absorbed by
  the FIFO and produces no artifact). It cannot paint black.
- **Nothing in the driver checks data correctness** — only arrival timing.
  Wrong-but-on-time bytes are structurally invisible to `underflows`.

The vendored components/esp_lcd override exists because stock IDF leaks an
in-flight slot when a flagged transaction's result is consumed (PATCH.md);
`panelFlush()` counts underflows and re-flushes once.

## Power management vs the flush

- The SPI driver takes `ESP_PM_APB_FREQ_MAX` per queued chunk on ESP32-S3.
  On ESP32-P4 IDF takes `ESP_PM_CPU_FREQ_MAX` instead, commented "to ensure
  PSRAM bandwidth and usability during DFS" (`spi_common.c:909-921`) —
  IDF's own authors saying DFS can disturb PSRAM, with the guard NOT
  extended to S3.
- All 11 chunks are queued up front, so the lock refcount never hits zero
  mid-frame: light sleep and sub-APB modes are excluded for the whole
  flush. PSRAM half-sleep exit latency therefore cannot hit mid-flush.
- On S3, `PM_MODE_APB_MAX` = 80 MHz CPU, so **every flush that follows an
  idle period forces a live CPU-clock transition right as chunk 1 is
  queued** — and `SOC_MEMSPI_CORE_CLK_SHARED_WITH_PSRAM=y`: the PSRAM
  clock hangs off the same core clock tree.

## OPEN BUG: persistent narrow black rows (task #21)

Symptom: black (zero-fill) bands a few px high, persist until next redraw,
`underflows` does NOT tick. Per the sections above the bands must be
transmitted zeros — the corruption sits between "correct bytes in PSRAM"
and "bytes latched into the SPI FIFO". Ranked hypotheses (desk research
2026-08-15, unconfirmed — do not treat as fact):

1. **Leading:** PSRAM read corruption coincident with the flush-start
   80 MHz relock (shared clock tree, see above). Predicts bands cluster in
   the top ~90 rows and follow idle gaps (first flush after no drawing).
2. Sub-detection descriptor stall with a zero-substitution path not found
   in the S3 GDMA HAL (weakened by the TRM master-mode-stalls fact).
3. `esp_cache_msync` coherency gap (call site looks correct; kept only as
   the other place "GDMA reads something other than what was drawn" could
   originate).

Planned instrumentation (one soak, decisive): per-chunk XOR/CRC of
`s_frame` computed before AND after each flush (mismatch = firmware-side
corruption; match + black band = read/DMA/wire path), plus per-flush
CPU-freq and time-since-last-flush tags, plus mapping any caught band's
row range to chunk index (`row*736/32768`): chunk-boundary = H1,
mid-chunk ~5.6-row granularity = H2. Light-sleep callbacks deliberately
NOT instrumented (excluded by the lock analysis above).

When #21 is resolved, replace the hypothesis list with the confirmed
mechanism and keep the detector-semantics sections — they are
version-pinned facts, not theories.
