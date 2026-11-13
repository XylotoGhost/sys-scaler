# sys-scaler

**Correct 16:9 picture on non-16:9 monitors for the docked Nintendo Switch.**

When docked, the Switch always outputs 16:9, and most 16:10, 4:3 or ultrawide monitors stretch that across the whole panel, so everything looks squeezed or stretched. sys-scaler fixes this from the Switch side: it adds black bars so the picture keeps its proper shape, centered on any monitor.

- Set it up once per monitor with a guided test in the app. It's remembered, and other monitors and TVs aren't touched.
- A background service applies it automatically whenever that monitor is docked, and turns it off when you undock.
- Works with taller monitors (16:10, 3:2, 4:3, 5:4: bars top and bottom) and wider ones (21:9, 32:9: bars left and right).
- Safe to try: every change shows a 15-second countdown and reverts unless you confirm it. Handheld mode is never affected.

> Status: early release (v0.5). Tested on an original Switch (HOS 23.0.1, Atmosphère 1.12.1) with the official dock and a 16:10 monitor. Reports from other monitors, especially ultrawides, are very welcome.

## Requirements
- A Switch running [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) custom firmware.
- Docked output to a monitor or TV that stretches the 16:9 signal to fill its screen.

## Install
1. Download `sys-scaler-vX.Y.Z.zip` from [Releases](../../releases).
2. Extract it to the root of your SD card. It contains `atmosphere/` and `switch/`.
3. Reboot.

To update, do the same. Your settings in `sdmc:/config/sys-scaler/` are kept.

## Use
1. Dock the Switch with the monitor connected.
2. Open **sys-scaler** from the Homebrew Menu. Launch it via a game (hold R) for full memory.
3. Choose **Set up this monitor**:
   - **Step 1:** a test circle appears. If it's already round, your monitor is fine and nothing changes.
   - **Step 2:** if not, sys-scaler applies the fix. Press **A** within 15 seconds to keep it.
   - **Check:** confirm the circle is round now, and it's saved.
4. Close the app. The fix now applies every time this monitor is connected.

### Easy and Expert mode
**Easy mode** has setup, on/off and "forget this monitor". **Expert mode** adds:

| Option | What it does |
|---|---|
| Method | **Centered** (default) keeps the normal signal and squeezes the picture: centered, works everywhere, slightly softer. **Sharp** sends a 16:10 signal (1920×1200): pixel-sharp, but with the bar at the bottom only. **Off** leaves the monitor alone. |
| Monitor shape | Auto reads it from the monitor. Override it if the circle isn't round. |
| Fine-tune picture | Move and resize the picture with a live preview. |
| Re-send interval | How often the fix is re-applied (default 1 s; the system resets it after boot and in the power menu). |
| Start at boot / Background service | Atmosphère boot2 flag; start or restart the service. |
| Saved monitors | List and delete saved monitors. |
| Export diagnostics | Writes monitor and dock details to `sdmc:/config/sys-scaler/diag/` for bug reports. |

## Good to know
- **Picture quality:** Centered scales the picture slightly, so it's a bit softer than native. Sharp is pixel-exact but not centered.
- **Loading animation:** the spinning Switch logo shown while loading stays stretched. It's drawn on a system layer the fix can't reach.
- **System Zoom:** while docked with a Centered monitor, sys-scaler takes over the screen zoom (it's the same mechanism as the accessibility Zoom feature).
- **Output resolution:** Centered works with the TV resolution set to 1080p or 720p. Sharp needs 1080p.
- **Dock limits:** the original dock tops out around 300 MHz pixel clock, so Sharp is limited to modes like 1920×1200@60.

## Uninstall or turn off
- Turn it off: set **Picture fix** to Off in the app.
- Remove it: delete `atmosphere/contents/420000000000A5F1/`, `switch/sys-scaler/` and `config/sys-scaler/` from the SD card.
- If the picture ever looks wrong, undock (handheld is never affected) and use **Reset picture now** or **Delete all saved monitors** in the app.

## Build
Needs Docker. From the repo root:
```sh
docker run --rm -v "${PWD}:/src" -w /src devkitpro/devkita64:latest make dist
```
This produces `sd/` (the SD card layout) and `dist/sys-scaler-vX.Y.Z.zip`. GitHub Actions builds every push and attaches the zip to tagged releases (`v*`).

| Path | What |
|---|---|
| `app/` | The sys-scaler app (.nro): UI, setup wizard, diagnostics |
| `sysmodule/` | Background service (title ID `420000000000A5F1`) |
| `common/` | Shared code: nvdrv/nvdisp access, vi magnification, EDID, aspect math, profiles, settings |
| `docs/research.md` | How it works, and the device findings behind it |
| `results/` | Notes from each device test run |

## Credits
Built on [libnx](https://github.com/switchbrew/libnx) and the research of the [switchbrew](https://switchbrew.org) wiki. Mode switching follows the approach of [SaltyNX](https://github.com/masagrator/SaltyNX)'s display refresh-rate code.

This project isn't affiliated with Nintendo.
