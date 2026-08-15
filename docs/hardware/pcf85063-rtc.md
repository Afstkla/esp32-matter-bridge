# PCF85063 — the real-time clock

Stub. Present on the board, **unused by this firmware.**

- I2C `0x51` — confirmed by our own I2C scan when the bus was first mapped.

Nothing in `main/` talks to it. Time comes from the network via Matter/Wi-Fi,
so nothing has needed it yet.

Add findings here the first time anyone actually reads a register off it.
