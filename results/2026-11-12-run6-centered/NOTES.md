# Run 6: modetest 0.4.0 + aspectfixd 0.2.0, CENTERED profile (2026-11-12)

- CENTERED (stock 1080p + zoom (0,-60,1920,1200)) rated "good" every time and saved
  as profile F7BCE59D (zoom). Works in general.
- Problem 1 (Moritz): sometimes the zoom stays after undocking -> bars in handheld.
- Problem 2 (Moritz): redock, or holding POWER for the power menu while docked,
  mostly resets the zoom -> distorted again.

aspectfixd.txt (only the last boot; the log was truncated at boot):
- applied zoom at 108.6 s, "disconnected" at 109.6 s (redock), then reconnected in a
  transient 720x480 state with a 13-mode DB -> key AEED4E52 -> no profile, and it
  never looked again once 1080p came back => problem 2 after redock.
- 10 s re-send interval => power-menu reset visible for up to 10 s.

modetest.txt: 3 of 5 runs have no "Exit." -> modetest was closed via HOME, its
cleanup never ran and sdmc:/aspectfix/pause stayed -> aspectfixd stayed paused
(no re-apply, and no zoom reset on undock while the zoom from modetest's own test
was still active) => problem 1 and 2.
Also undock detection relied on disp1 GET_MODE2 going inactive, which can lag.

Fixes in aspectfixd 0.3.0 / modetest 0.4.1: omm-based dock detection every 250 ms
with immediate + repeated zoom reset; pause file holds modetest's pid and is
ignored/removed once that process is gone (and at boot); profile re-lookup after
every settled mode change and every 3 s while none is found; zoom re-sent every 1 s;
log appended across boots (capped at 128 KB).

log.txt/edid/modedb: a diag 0.3.0 run (unchanged results).
