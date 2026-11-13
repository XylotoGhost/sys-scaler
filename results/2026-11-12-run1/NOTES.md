# Run 1: AspectFix Diag 0.2.0, read-only diagnostic (2026-11-12)

Original Switch (Erista), HOS 23.0.1, Atmosphère, official dock, Acer PE160WUT (2880x1800, 16:10).

- **nvdrv permissions:** `nvdrv` and `nvdrv:a` open fine but are denied `/dev/nvdisp-ctrl`, `-disp0` and `-disp1` (nv 0x30003). `nvdrv:s` gets all three, from an .nro.
- **Live mode:** disp1 is 1920x1080@60, 148.5 MHz.
- **Mode DB:** 22 modes. It holds the 16:10 modes 2880x1800 (EDID detailed timing, 336.5 MHz), 1920x1200 (154.1 MHz), 1680x1050, 1440x900 and 1280x800. The field meanings in `common/nvdisp.h` were decoded from this dump.
- **VALIDATE_MODE2:** accepted every candidate, including 2880x1800 and generated CVT-RB timings, so it doesn't check the dock's limit. It sets `vmode` to 0x8000000 in the struct it returns.
- **"Live mode unchanged: NO" was a false alarm.** The timings before and after were identical; only `bpp` (16 vs 0) and `unk1` (0x20021300 vs 0x3) differed between two `GET_MODE2` calls. Those two fields are garbage, so only timings can be compared.
- **Dock:** DP link HBR2 x 2 lanes; converter OUI 0080e1, id "Dp1.1". DPCD 0x080 = `0b 78`: HDMI output, max TMDS 0x78 x 2.5 = 300 MHz. So 2880x1800@60 is out and 1920x1200@60 fits.
- **AVI infoframe:** VIC 16, picture aspect 0 (no data).
- **Panel aspect:** 16:10, from the EDID's native resolution.

Conclusion: a native-aspect output mode looks possible. Next: SET_MODE2 with 1920x1200 and auto-revert (run 2).
