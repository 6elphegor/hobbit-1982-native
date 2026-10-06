Vendored from https://github.com/superzazu/z80 (MIT, see LICENSE).

Local patch: port_in/port_out take the full 16-bit port address
(B or A as the high byte), which ZX Spectrum keyboard reads require.
