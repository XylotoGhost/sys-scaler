# Run 5: modetest 0.3.1, vi display magnification in 1920x1200 mode (2026-11-12)

All four SetDisplayMagnification calls returned rc 0 on display "Default" (id 19,
logical 1920x1080). Photos show the HOME menu (Moritz pressed HOME during the test):

- centerattempt.jpeg, (0,-60,1920,1080): image moved down ~60 lines, but the bottom
  60 lines are cut off at row 1080 -> vi clips to the 1920x1080 display it set.
  The area below 1080 stays black. Centering inside the 1200 mode is impossible.
- "going up.jpeg", (0,+60,1920,1080): image moved up, top cut off -> sign confirmed.
- RECTother*.jpeg, (0,0,1920,1200) / (0,-60,1920,1200): image squashed vertically
  (1200 logical lines mapped onto 1080), i.e. vi maps the rect non-uniformly.

Conclusion: magnification = arbitrary non-uniform scale + offset, clipped to 1080.
-> In the STOCK 1080p mode, rect (0,-60,1920,1200) draws the image at 1920x972 +0+54,
   which the monitor's 1080->1200 stretch turns into a correct, centered 16:9 picture
   (approach A done by vi). Implemented as profile method "zoom" (modetest 0.4.0,
   aspectfixd 0.2.0).
