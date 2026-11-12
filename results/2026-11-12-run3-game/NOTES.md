# Run 3: modetest 0.2.0 + aspectfixd 0.1.0 (2026-11-12)

- modetest: 1920x1200 confirmed, then "-" (keep without profile) and exit.
- Smash Bros. Ultimate (IMG_1284) runs at 1920x1200: correct proportions,
  image top-aligned, black bar below. The mode survived launching the game.
- No profile was saved, so aspectfixd never applied anything. Its log only
  shows pause/resume/disable, which works as intended.
- Second modetest run (handheld): recovery menu, deleted 0 profiles, then
  disabled aspectfixd (the `disabled` flag file from the SD card is included here).
- Moritz: bar only at the bottom looks odd -> wants an option to center.
