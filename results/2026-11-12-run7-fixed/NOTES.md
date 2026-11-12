# Run 7: aspectfixd 0.3.0 (CENTERED profile F7BCE59D)

Moritz: "all looks good so far", no stutter from the 1 s zoom refresh.

Log (`aspectfixd.txt`, top part is the earlier 0.2.0 boot from run 6):
- Boot undocked: "undocked (omm)" at 11 s, no stale pause file.
- Dock at 47.7 s → disp1 1920x1080 at 48.0 s → profile found at once → zoom set at 51.0 s (3 s settle).
- Undock at 157.5 s → zoom reset 9 ms later (omm). No zoom left in handheld.
- Redock at 170.7 s → profile found → zoom at 173.9 s. The transient 480p state didn't occur this time.
- Undock at 229.3 s caught via "disp1 inactive" 250 ms before omm reported it.
- The silent 1 s re-sends don't log, so the log can't show whether the power menu reset anything.

Observation: after docking, the picture stays unzoomed for about 3 s (SETTLE_MS).
