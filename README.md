# switchscaler: docked aspect-ratio fix

Goal: show a correct 16:9 picture with black bars on any non-16:9 monitor connected through the official dock, fixed from the Switch side. The first target is an Acer PE160WUT (16:10, 2880×1800) on an original Switch (Erista), HOS 23.0.1, Atmosphère 1.12.1.

**Status:** two methods, chosen per monitor in modetest:
- **CENTERED** (`zoom`): keeps the stock 1080p output. vi's display magnification (the system Zoom feature) pre-squashes the image to 1920×972 at y+54, and the monitor's own stretch makes it 16:9 again with bars top and bottom. Slightly softer. Needs no mode change and works within any dock bandwidth. The vi mechanics were confirmed in run 5; the combination still needs a device test.
- **SHARP** (`mode`): outputs 1920×1200, and vi draws the image 1:1 at the top with a 120-line bar below. Confirmed in runs 2–3, including in games.

The `aspectfixd` sysmodule applies the saved one automatically.

## Layout
| Path | What |
|---|---|
| `common/` | Shared code: own nvdrv client, nvdisp structs, EDID parser, aspect math, DPCD/dock info, profiles, logging, `app.mk` build rules |
| `diag/` | Read-only diagnostic (.nro) |
| `modetest/` | Tries native-aspect modes with auto-revert and saves a confirmed one as a profile (.nro) |
| `sysmodule/` | `aspectfixd`, applies the saved profile in the background (title ID `420000000000A5F1`) |
| `results/` | Logs and photos from the device |

## Build (Windows, Docker Desktop)
From the repo root:
```powershell
docker run --rm -v "${PWD}:/src" -w /src devkitpro/devkita64:latest make
```
This builds all three and assembles `sd/`. Copy the contents of `sd/` to the SD card root:
```
sd/switch/aspectfix/aspectfix-diag.nro
sd/switch/aspectfix/aspectfix-modetest.nro
sd/atmosphere/contents/420000000000A5F1/exefs.nsp
sd/atmosphere/contents/420000000000A5F1/flags/boot2.flag
```

## How it adapts to any monitor
Games always render 16:9, and vi composes a 1920×1080 image. The only input is the **panel aspect**. It's taken from the monitor's EDID native resolution, falling back to its physical size, and `sdmc:/aspectfix/aspect.txt` (e.g. `16:10`) overrides it. CENTERED computes the zoom rect so the image lands on the A rectangle (16:10 → zoom `(0,-60,1920,1200)`, 4:3 → `(0,-180,1920,1440)`, 21:9 → `(-320,0,2560,1080)`). SHARP picks the smallest mode in that aspect that holds 1920×1080 unscaled ([aspect.c](common/aspect.c)):

| Panel | A / CENTERED: keep 1080p, pre-squash | B / SHARP: native-aspect signal |
|---|---|---|
| 16:10 | game 1920×972 at y+54 | out 1920×1200, game 1:1 at y+60 |
| 3:2 | 1920×912 at y+84 | 1920×1280, y+100 |
| 4:3 | 1920×810 at y+135 | 1920×1440, y+180 |
| 21:9 (64:27) | 1440×1080 at x+240 | 2560×1080, x+320 |
| 32:9 | 960×1080 at x+480 | 3840×1080, x+960 |

Profiles are keyed per display (CRC of the driver's mode DB, which comes from the EDID), so a normal TV is never touched. The B mode must also fit the dock converter's TMDS limit (300 MHz on the original dock).

## Results so far
**Run 1, diag** ([log](results/2026-11-12-run1/log.txt)):
- An `.nro` can open `nvdrv:s` and all nvdisp devices.
- The driver mode DB, built from the EDID, includes 1920×1200@60 CVT-RB (154.128 MHz), and `VALIDATE_MODE2` accepts it.
- The dock is HDMI with max TMDS 0x78 × 2.5 = **300 MHz**, so native 2880×1800@60 (336.5 MHz) is out. The DP link is HBR2 × 2.
- `NvdcMode2` decoding: `sync` bit0 = +H, bit1 = +V. `vmode` 0x200000 = EDID DTD, 0x400000 = CEA, 0 = DMT. `unk1` = CEA aspect flag (0x40 = 4:3, 0x80 = 16:9). `GET_MODE2` returns garbage in `bpp` and `unk1`, so compare timings only.

**Run 2, modetest 0.1.0** ([notes](results/2026-11-12-run2-modetest/NOTES.md)):
- `SET_MODE2` 1920×1200 works through the dock. vi draws its 1920×1080 image **top-left and unscaled**, leaving a black strip below, and the proportions are correct.
- HOME didn't reset the mode.
- Modes smaller than 1920×1080 crop the image and looked badly distorted, so modetest no longer offers them.

**Run 3** ([notes](results/2026-11-12-run3-game/NOTES.md)): games run fine at 1920×1200, but the bar sits at the bottom only.

**Run 5, display magnification** ([notes](results/2026-11-12-run5-zoom/NOTES.md)):
- vi accepts any rect, including negative offsets and sizes bigger than the display, and maps it non-uniformly.
- It clips to the 1920×1080 display it set, so in 1920×1200 a shift down just loses the bottom 60 lines.
- That led to CENTERED: stock 1080p plus a squash rect.

## modetest 0.4.0
1. Dock, then launch via hbmenu (hold R on a game).
2. The list shows **CENTERED** first, then the **SHARP** modes (1920×1200 …). Select one and press **A**. Press **X** within 15 s if you can read the screen; otherwise it reverts by itself. You can press HOME during the countdown to look at real content.
3. **Y** saves it as this monitor's profile. **+** undoes it. For SHARP, **−** keeps the mode without a profile and exits.
4. Menu: **R** restores 1080p and resets the zoom, **L** deletes this monitor's profile, **ZR** toggles aspectfixd (`sdmc:/aspectfix/disabled`).
5. In handheld, the app offers recovery: **Y** deletes all profiles, **X** resets the zoom, **ZR** toggles aspectfixd.

While modetest runs it creates `sdmc:/aspectfix/pause`, so aspectfixd stays out of the way and resets its zoom.

## aspectfixd 0.4.0 (sysmodule)
- **Every 250 ms:** it reads the operation mode from `omm`. On undock it resets the zoom immediately and again after 1 s and 3 s, because in handheld "Default" is the built-in screen.
- **Every 1 s while docked:** it reads the disp1 mode. It looks up the profile (by mode-DB CRC) after every mode change once things have settled for 1 s, and every 3 s while none is found. This is needed because docking first goes through a temporary 480p state with a different, 13-mode DB.
- **zoom profile:** once the base 1920×1080 mode has been live for 1 s, it sends the magnification rect. It re-sends it every N seconds, where N comes from `sdmc:/aspectfix/refresh.txt` (default 1, `0` = only after dock or mode events). vi has no getter, so a reset by something else can't be detected, only overwritten. It resets the zoom on any other mode, such as 720p.
- **mode profile:** once the base mode has been live and stable for 3 s, it validates and sets the profile mode. If the system resets it, it re-applies up to 5 times in a row.
- **Pause file:** it holds modetest's pid. aspectfixd removes it once that process is gone (for example when modetest was closed via HOME), and also at boot.
- It never touches monitors without a profile.

Log: `sdmc:/aspectfix/aspectfixd.txt`, appended across boots and restarted above 128 KB.

Run 6 ([notes](results/2026-11-12-run6-centered/NOTES.md)): CENTERED works in general. Undocking sometimes kept the zoom in handheld, and redocking or the power menu reset it. Root causes: a leftover pause file, slow and unreliable dock detection, a single profile lookup during the temporary 480p state, and a 10 s refresh. All of these are fixed in 0.3.0.

Run 7 ([notes](results/2026-11-12-run7-fixed/NOTES.md)): 0.3.0 handled undock and redock correctly, with no stutter from the 1 s refresh. 0.4.0 makes the refresh configurable and cuts the wait after docking from 3 s to 1 s.

Run 8 ([notes](results/2026-11-13-run8-refresh0/NOTES.md)) ran with the refresh off. The zoom held through sleep/wake, games, the HOME menu and redock. It was lost after a fresh boot (the HOME menu startup resets it after aspectfixd sends it) and after the power menu. aspectfixd sees no event for either, so the 1 s re-send (the default) stays. It also overrides the system Zoom accessibility feature while docked.

To turn it off, delete the profile, create `sdmc:/aspectfix/disabled`, or remove `atmosphere/contents/420000000000A5F1`.

## Open questions
- CENTERED end to end: does the image look right, and how soft is it?
- Does vi keep the magnification when an app or game starts, and through sleep?
- The spinning Switch-logo loading indicator stays 16:10, so it seems to be on a layer that magnification doesn't apply to. That's a cosmetic limitation of CENTERED.
- Could SHARP also be centered? That would need vi to compose into 1200 lines, which means patching vi at runtime (firmware-specific). An `nvdrv:s` FLIP MITM probably isn't feasible, because vi opens its session before boot2.

## Interface notes
- **Mode struct field order** is `hActive, vActive, hSyncWidth, vSyncWidth, hFrontPorch, vFrontPorch, hBackPorch, vBackPorch, …`, not the Linux `tegra_dc_mode` order. Current FW uses the 60-byte `MODE2` ioctls (`0x803C021B` get, `0x403C021C` set, `0xC03C021D` validate, `0xEF20021E` mode DB).
- **Permissions:** nvdisp devices need NvDrvPermission bit 8 (Display), which only `nvdrv:s` (0x439E) and `nvdrv:t` have. The tools open their own `nvdrv:s` session.
- **Precedent:** SaltyNX's `saltysd_proc` changes the docked refresh rate with `GET_MODE2` → `VALIDATE_MODE2` → `SET_MODE2` on `/dev/nvdisp-disp1`. FPSLocker/SaltyNX only touch 480p/720p/1080p modes, so they leave 1920×1200 alone.

References: switchbrew "NV services", masagrator/SaltyNX `saltysd_proc/source/display_refresh_rate.cpp`, masagrator/sys-dock, masagrator/FPSLocker.
