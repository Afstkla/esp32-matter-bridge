#pragma once

#include <cstdint>

#include "panel.h"

// Drawing goes straight into the panel's big-endian RGB565 framebuffer, so
// every colour must come from theme.h — a native-endian literal renders as the
// wrong hue.
void gfxInit(uint16_t *frame);

void gfxFillScreen(uint16_t colour);
void gfxFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t colour);
void gfxDrawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t colour);

void gfxFillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t colour);
void gfxDrawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t colour);
void gfxFillCircle(int16_t cx, int16_t cy, int16_t r, uint16_t colour);

// font8x8 glyphs sit in an 8x8 cell, so text is 8 * scale per character wide
// and 8 * scale tall — wider per character than the old 6x8 Arduino font.
void gfxText(int16_t x, int16_t y, const char *text, uint16_t colour, uint8_t scale);
int16_t gfxTextWidth(const char *text, uint8_t scale);
