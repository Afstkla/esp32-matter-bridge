# Local esp_lcd override

Verbatim copy of ESP-IDF v5.5.5 `components/esp_lcd` (test_apps dropped) with
one change, in `spi/esp_lcd_panel_io_spi.c`: the five transaction-drain loops go
through `lcd_spi_recycle_trans()`, which treats `ESP_ERR_INVALID_STATE` from
`spi_device_get_trans_result()` as "recycled with a fault" instead of aborting
the drain, and counts those faults. The count is readable through
`esp_lcd_spi_underflow_count()`, declared in `include/esp_lcd_io_spi.h` — the
only other file this override touches.

Why: a PSRAM-sourced chunk that hits a DMA TX underflow still completes and
still returns its result — flagged, so `get_trans_result` reports
`ESP_ERR_INVALID_STATE` (spi_master.c:1337). The stock drain loop bails on that
error *after* consuming the result but *without* decrementing
`num_trans_inflight`, so the counter runs one ahead of reality forever and the
next drain blocks in `portMAX_DELAY` on a result that will never arrive. On
this board that was an intermittently but reproducibly wedged ui task, readily triggered with DFS enabled. With the patch, an underflow costs one
corrupted 32 KB chunk on the wire, a `W` log line, and — because the count tells
`panelFlush()` the frame it just sent is damaged — one extra ~18 ms flush.

The count advances in the drain loops, i.e. in whatever task called into the
panel IO, never in an ISR — on this firmware only ever the ui task, since the
panel is single-task-owned. Readers are another matter: the `power` console
command prints it from whichever task ran the command, the USB REPL or a network
console session. One writer and cross-task readers of a `volatile uint32_t` need
no lock on this target because an aligned 32-bit load or store is atomic; a
reader can be one increment stale, which is all a diagnostic counter owes anyone.

Remove this override when upstream ESP-IDF fixes the drain accounting; an upstream issue against ESP-IDF is planned.
