# How sys-scaler works (research notes)

These are the technical findings behind sys-scaler, from tests on an original Switch (Erista) on HOS 23.0.1 with Atmosphère 1.12.1, through the official dock, on an Acer PE160WUT (16:10, 2880×1800). Raw notes for each device run are in [results/](../results).

## The problem
The Switch always outputs a 16:9 signal when docked (1920×1080 or 1280×720). Most monitors stretch that signal across their whole panel. On a 16:10 panel, everything looks about 11% too tall. On an ultrawide, it looks too wide. Many monitors have no "keep aspect ratio" option.

## Two ways to fix it from the Switch side

| Panel | Centered: keep 16:9 output, pre-squash | Sharp: send a signal in the panel's own shape |
|---|---|---|
| 16:10 | picture 1920×972 at y+54 | output 1920×1200, picture 1:1 at the top |
| 3:2 | 1920×912 at y+84 | 1920×1280 |
| 4:3 | 1920×810 at y+135 | 1920×1440 |
| 21:9 (64:27) | 1440×1080 at x+240 | 2560×1080 (picture stuck at the left, not offered) |
| 32:9 | 960×1080 at x+480 | not offered |

**Centered** uses vi's display magnification, `ISystemDisplayService` cmd 1204 `SetDisplayMagnification`, which is the call behind the system Zoom feature. vi maps a source rect onto the whole display, and that mapping doesn't have to keep proportions. Passing a rect larger than the screen therefore shrinks the picture along one axis, for example 16:10 → `(0, -60, 1920, 1200)`. The monitor's own stretch then restores the correct proportions, with bars where the shrink left space. It works for any panel shape and any dock, because the signal doesn't change. The cost is a slight softness.

**Sharp** switches the output mode with nvdisp `SET_MODE2`, to 1920×1200 for 16:10. vi keeps composing 1920×1080 and draws it unscaled at the top-left. The picture is pixel-exact, but it has a 120-line bar at the bottom only. vi clips magnification to its own 1080 lines, so the picture can't be centered in that mode. Modes smaller than 1920×1080 crop the picture and showed heavy artefacts, so they're excluded.

The panel shape comes from the EDID's native resolution, falling back to its physical size. Expert mode can override it.

## Device findings
- **nvdisp access:** `/dev/nvdisp-disp1` needs NvDrvPermission bit 8, which only `nvdrv:s` and `nvdrv:t` have (run 1: `nvdrv` and `nvdrv:a` are denied). libnx picks one nv service per process, so both the sysmodule and the app set `__nx_nv_service_type` to `nvdrv:s` and use libnx's own `nvOpen`/`nvIoctl`. In the app that puts the framebuffer on `nvdrv:s` too; it needs only nvmap and nvhost-ctrl, which no permission bit gates.
- **Mode struct order:** `hActive, vActive, hSyncWidth, vSyncWidth, hFrontPorch, vFrontPorch, hBackPorch, vBackPorch, pclkKHz, bpp, vmode, sync, unk1, reserved`. Current firmware uses the 60-byte `MODE2` ioctls: `0x803C021B` get, `0x403C021C` set, `0xC03C021D` validate, `0xEF20021E` mode DB.
  - `sync`: bit 0 = +H, bit 1 = +V.
  - `vmode`: 0x200000 = EDID DTD, 0x400000 = CEA, 0 = DMT.
  - `unk1`: CEA aspect flag.
  - `GET_MODE2` returns garbage in `bpp` and `unk1`, so compare timings only.
- **Dock limit:** the original dock's DP→HDMI converter reports max TMDS 0x78 × 2.5 = 300 MHz (DPCD 0x081). So 2880×1800@60 (336.5 MHz) is out, while 1920×1200@60 CVT-RB (154.1 MHz) works.
- **vi session:** an app's window already holds the "Default" display on libnx's vi session, and opening it twice fails with 0x1272. So magnification uses its own `vi:m` session ([vimag.c](../common/vimag.c)).
- **vi accepts any rect,** including negative offsets and sizes bigger than the display. There's no getter.
- **Things that reset the magnification:** the HOME menu starting after boot, and the power menu. Neither produces an event a sysmodule can see, so the zoom is re-sent every second (run 8). Sleep/wake, games, the HOME menu and redocking keep it.
- **Docking passes through a temporary 720×480 state** with a different, 13-mode DB. The profile lookup therefore repeats until the real mode has settled (run 6), and the app re-reads the monitor whenever the DB changes.
- **Monitor identity:** a profile is keyed by the timings in the driver's mode DB (a sum of per-mode CRCs), not by the raw DB bytes, so the DB's order and its flag and unknown fields don't matter.
- **Docked or not:** `omm` `GetOperationMode` (cmd 0) is the reliable docked/handheld signal. disp1 going inactive can lag.
- **System layers:** the spinning loading indicator is drawn on a layer that magnification doesn't affect, so it stays stretched with Centered.

## Device runs
| Run | What | Result |
|---|---|---|
| 1 | read-only diagnostic | nvdrv:s works from an .nro; the mode DB has 1920×1200; dock is HDMI at 300 MHz |
| 2–3 | SET_MODE2 1920×1200 | works in the menu and in games; picture at the top, bar at the bottom |
| 4–5 | display magnification | rect mapping understood; clipped at 1080 in 1200 mode → Centered = 1080p + squash |
| 6 | Centered via sysmodule | works; stale pause file and missed lookups fixed |
| 7 | 1 s re-send | undock/redock correct, no stutter |
| 8 | re-send off | boot and power menu lose it → 1 s stays the default |
| 9 | app 0.5.0 (UI, wizard) | works; finishing setup now also turns the fix on |

## Precedent and references
- SaltyNX `saltysd_proc` changes the docked refresh rate with `GET_MODE2` → `VALIDATE_MODE2` → `SET_MODE2` on `/dev/nvdisp-disp1`.
- switchbrew: "NV services", "Display services".
- masagrator/SaltyNX, masagrator/sys-dock, masagrator/FPSLocker.
