# Run 9: sys-scaler 0.5.0 app + sysmodule

Moritz: "all looks great so far". The one bug: after setting up, the menu still showed Picture fix as off although the fix was visibly active.

Screenshots (captured before magnification, so the circle looks round in them):
- The UI renders correctly with the system font.
- The Nintendo button glyphs work: A (U+E0E0), B (U+E0E1), X (U+E0E2), + (U+E0EF).
- The wizard screens look as designed.

Logs:
- The sysmodule's first start reported "0 prototype profile(s) taken over". That was correct: Moritz had already deleted `sdmc:/aspectfix`. The copy-over itself remains untested on device.
- `enabled` was switched off while still in handheld (rev 2), then toggled on and off several times while docked. It ended at `enabled=0`.
- The setup wizard saved the profile but left `enabled=0`. The app's test zoom stayed on screen, and the disabled service never reset it, because its own zoom wasn't active. So the picture looked fixed while the menu correctly said "Turned off", and after a reboot nothing would apply.

**Fix (0.5.1):** saving a Centered or Sharp profile turns Picture fix on.
