# Predictive Input Buffer

When you click a little too early and a hidden ghost run says that click kills you, the click is held back a few ticks (as few as possible) and fired when it's safe. Works for presses and, in wave / robot (and ship if you turn it on), early releases.

After a death, the **coach** plays your last moments again with the click a little earlier / later and tells you how far off you were ("click 13 ms late").

- **F6** cycles the buffer: 2 -> 4 -> 8 -> 16 ticks -> off (wave stops at 8).
- **F7** shows the window (hidden from screen capture). **LIVE** tab: your last death's timing in big, a bar of every death from "too early" to "too late", where in the level you die, and a scrolling log. **SETTINGS** tab: buffer range, randomize, per-mode limits and every switch.

v3.1.0: new window (LIVE / SETTINGS tabs, timing bar, death map, every switch in the window, scrolling log).

v3.0.0 is a full rewrite:
- **Coach**: after each death, how early or late your click / release was, in ms and ticks. Works with the buffer switched off too, so you can practise raw and still see your timing. Per level: your average early / late, and the spot you die at most.
- When a click can't be fixed within your limit, the log says how much delay it would have needed ("38 ms early: needed +9, limit 4").
- **Self-check on every click** the ghost looks at, not only held-back ones, plus whether you died when the ghost said you would ("ghost right 17/18").
- The ghost gets exactly the step size the real player gets (also in time warp), and a click that Click Between Frames lands late in a step keeps its real timing.
- Pads work from the first one (no need to hit each type once), in every gamemode.
- Fixed: a ghost run could set off a level's event triggers or ask for a practice checkpoint.
- Everything from 2.x is still there: randomize, per-mode limits, finer timing, best fix, only-when-sure, hold fix, your taps, moving objects, pads / portals / orbs, death check.
