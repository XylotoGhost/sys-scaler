# Run 8: aspectfixd 0.4.0 with refresh.txt = 0 (zoom sent only after dock or mode events)

What Moritz saw:
| Scenario | Result |
|---|---|
| Fresh boot (docked) | **Not applied.** Reapplied via modetest |
| Hold POWER → power menu | **Breaks it.** Reapplied via modetest |
| Sleep / wake | OK |
| Start and close a game | OK |
| HOME menu open/close | OK |
| Undock / redock | OK |

He also noticed that the spinning Switch-logo loading indicator always shows in 16:10, i.e. stretched and not zoomed.

From the log:
- At boot the zoom was sent at 14.4 s (rc 0), right after docking at 12.5 s, yet it wasn't visible. So the system (HOME menu startup) resets the magnification after that.
- Holding POWER produces no event aspectfixd can see: no omm change, no disp1 change. The system resets the magnification silently.
- Sleep/wake shows up as disp1 inactive → active at 134.0–135.0 s, and the zoom was re-sent 1 s later.
- vi has no getter, so these resets can only be overwritten periodically, not detected.

**Conclusion:** the periodic re-send is required. The default stays at 1 s, which caused no stutter in run 7. Delete `refresh.txt` to get the default.

Loading spinner: games and the HOME menu were zoomed correctly, so the spinner is most likely drawn on a system layer that the display magnification doesn't apply to. It can't be fixed with this method.
