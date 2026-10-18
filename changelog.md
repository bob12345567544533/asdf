# v1.0.0
- Buffer only: slider, Enabled / Releases / Ship / Timing popup toggles, window with log, F6 / F7 keys.
- The simulation stops at portals, pads, teleports and orbs (while holding) instead of ignoring them, so predictions near them stay correct.
- When several delays work, it uses one tick past the earliest for a safety margin.
- Capped simulation work per click, so a click that dies anyway can't cause a frame hitch.
