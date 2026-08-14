#pragma once

// The board's own sensors and controls, exposed as a single accessory made of
// several endpoints — a composed device, in Matter's terms. The parent carries
// the name; each child carries one function, and the parent's PartsList is what
// tells a controller they belong together.
//
// Unlike the accessories in builder.cpp these are not invented: the temperature
// is the PMU die, and the brightness really does drive the panel.
//
// Must run after esp_matter::start().
void internalsBegin();

// Publishes fresh readings. Rate limited internally; call it from loop().
void internalsPoll();
