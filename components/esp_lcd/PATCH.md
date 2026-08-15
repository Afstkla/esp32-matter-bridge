# Local esp_lcd override

Verbatim copy of ESP-IDF v5.5.5 `components/esp_lcd` (test_apps dropped) with
exactly one change: the five transaction-drain loops in
`spi/esp_lcd_panel_io_spi.c` go through `lcd_spi_recycle_trans()`, which treats
`ESP_ERR_INVALID_STATE` from `spi_device_get_trans_result()` as "recycled with
a fault" instead of aborting the drain.

Why: a PSRAM-sourced chunk that hits a DMA TX underflow still completes and
still returns its result — flagged, so `get_trans_result` reports
`ESP_ERR_INVALID_STATE` (spi_master.c:1337). The stock drain loop bails on that
error *after* consuming the result but *without* decrementing
`num_trans_inflight`, so the counter runs one ahead of reality forever and the
next drain blocks in `portMAX_DELAY` on a result that will never arrive. On
this board that was a permanently wedged ui task roughly one 40-swipe run in
three, near-certain with DFS enabled. With the patch, an underflow costs one
corrupted 32 KB chunk on the wire for one frame and a `W` log line.

Remove this override when upstream ESP-IDF fixes the drain accounting; a draft
issue lives in the private scratchpad UPSTREAM.md.
