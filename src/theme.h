#pragma once

#include <Arduino_GFX_Library.h>

// Colours are stored byte-swapped because the canvas is byte-swapped: the panel
// wants big-endian RGB565, and holding the framebuffer that way lets a frame go
// out by DMA untouched instead of being copied pixel by pixel into a bounce
// buffer on the way. See panelFlush().
//
// The cost is this file. A native-endian colour reaching the canvas shows up as
// a wrong hue, so no colour is written inline anywhere else.
constexpr uint16_t beColour(uint16_t native) {
  return (uint16_t)((native >> 8) | (native << 8));
}
#define BE_RGB565(r, g, b) beColour(RGB565(r, g, b))

// Both are symmetric, so the swap leaves them alone.
constexpr uint16_t COL_BLACK = 0x0000;
constexpr uint16_t COL_WHITE = 0xFFFF;

constexpr uint16_t COL_BG = BE_RGB565(28, 28, 32);
constexpr uint16_t COL_WELL = BE_RGB565(40, 40, 46);
constexpr uint16_t COL_OUTLINE = BE_RGB565(70, 70, 80);
constexpr uint16_t COL_FAINT = BE_RGB565(110, 110, 120);
constexpr uint16_t COL_MUTED = BE_RGB565(150, 150, 160);

constexpr uint16_t COL_BUTTON = BE_RGB565(70, 140, 240);
constexpr uint16_t COL_LEVEL = BE_RGB565(240, 160, 60);

constexpr uint16_t COL_OK = BE_RGB565(90, 220, 130);
constexpr uint16_t COL_ALERT = BE_RGB565(230, 90, 90);
constexpr uint16_t COL_DANGER = BE_RGB565(240, 90, 90);
constexpr uint16_t COL_WARN = BE_RGB565(255, 90, 90);
constexpr uint16_t COL_NET = BE_RGB565(90, 130, 90);
constexpr uint16_t COL_DIM = BE_RGB565(140, 140, 140);
