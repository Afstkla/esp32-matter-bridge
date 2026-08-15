# QMI8658 — the IMU

Stub. Present on the board, **unused by this firmware.**

- I2C `0x6B`.
- `WHO_AM_I` register `0x05`, chip ID `0x7C` — confirmed by our own I2C scan
  when the bus was first mapped.

Nothing in `main/` talks to it. It is a candidate **future wake source**
(motion/pick-up wake alongside the touch INT on GPIO21, see
[cst820-touch.md](cst820-touch.md)) — no work has been done on that, and no
interrupt line has been traced.

Add findings here the first time anyone actually reads a register off it.
