# Run 4: modetest 0.3.0 centering experiments (2026-11-12)

- modetest.txt is cumulative (contains runs 3 and 4); run 4 = the 0.3.0 section.
- 1920x1200 set and confirmed again.
- ZL (centering experiments) never got to SetDisplayMagnification:
  `viOpenDefaultDisplay failed 0x1272` (vi module 114, desc 9). libnx's vi session is
  Manager-level (both IManager/ISystem active), but the console window already holds
  "Default" on that session and a second OpenDisplay on it fails.
  -> 0.3.1 uses its own vi:m session (common/vimag.c).
- aspectfixd: still disabled (flag file present), nothing applied.
