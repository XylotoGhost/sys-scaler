# Run 2: modetest 0.1.0 (2026-11-12)

- 1920x1200@60 (driver DB[4], 154.128 MHz): monitor shows it. The system image
  (1920x1080) is drawn top-aligned and unscaled, with the black strip **below**
  (~120 lines). HOME menu photo IMG_1281 shows correct proportions.
  Note: the first "none (fills screen)" answer in modetest.txt was a mis-press;
  the second answer "below" is correct (confirmed by Moritz).
- HOME during the test did not reset the mode (no "changed by the system" entry).
- 1680x1050 (IMG_1282): the 1920x1080 layer is cropped, not scaled, and the
  monitor showed heavy distortion / colour interference. 1440x900 was not
  confirmed within 15 s. Modes smaller than 1920x1080 are unusable.
- IMG_1278: modetest menu at stock 1080p (stretched by the monitor).
