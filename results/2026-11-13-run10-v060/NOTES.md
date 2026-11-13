# Run 10: sys-scaler 0.6.0 (review changes)

Moritz: after applying a fix the picture instantly went back to distorted and the app showed "nothing to fix"; after the update and a reboot the fix was no longer applied. Sharp and the rest couldn't be tested.

**Cause: the profile key changed on every read.** 0.6.0 hashed each mode's timing by copying it into a local `u32` array and passing that to libnx's `crc32Calculate`. That function reads its input through `u64` pointers, which is undefined for a `u32` array, and GCC at -O2 dropped the copies. The disassembly shows the CRC running over uninitialised stack plus the pixel clock. The key therefore depended on the call path:

- Sysmodule, same monitor, same 22-mode DB: `77C5DC2B`, `AABDF207`, `2E92725F`, always "profile none".
- App: `8441F167`, `93EE276B`, `94440C67`, `BFBC1987`, `56B7E57F`.

Consequences seen in the logs:
- The wizard saved Centered under `93EE276B`; the sysmodule never computed that key, found no profile and reset the zoom as soon as the test ended. That's the instant return to the distorted picture.
- The app's once-a-second staleness check and its own init computed different keys, so it re-read the monitor every second (one "Monitor ..." line per second in app-log.txt).
- One wizard run was answered with "Yes, it's round" and saved Off under `93EE276B`; a later Centered save landed on `94440C67`. The app then showed the Off profile: "nothing to fix".
- After the update nothing applied because the 0.5 profile (`F7BCE59D`) uses the old key scheme. That part was expected; Moritz deleted it in this run.

**Fix (0.6.1):** the key is now a plain FNV-1a hash over the timing values, no libnx CRC. Checked on the PC against the mode DB dump from run 1: same key on repeated calls and with the modes reordered and flag fields scrambled, a different key when one timing or the mode count changes. Expected key for this monitor: `13B05411`.

What 0.6.0 did confirm on device:
- The app runs with its framebuffer on `nvdrv:s`, and libnx's nv calls work in both binaries.
- The sysmodule boots and works with the reduced NPDM (service list and SVC whitelist); pause detection via pm:info works.
- After a test ended with no matching profile, the sysmodule sent the identity zoom ("zoom no profile / off"), as intended.
- Sysmodule heap peak: 44 KB of 384 KB. 0.6.1 reduces the heap to 128 KB.

Leftover on the SD card: `profiles/93EE276B.ini` and `profiles/94440C67.ini` belong to no monitor; delete them under Saved monitors.
