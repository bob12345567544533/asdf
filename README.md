# Predictive Input Buffer

When you click a little too early and a hidden ghost run says that click kills you, the click is held back a few ticks (as few as possible) and fired when it's safe. Works for presses and, in wave / robot (and ship if you turn it on), early releases.

- **F6** cycles the buffer: 2 -> 4 -> 8 -> 16 ticks -> off (wave stops at 8).
- **F7** shows the window: buffer range, randomize, switches, saves per attempt and per level, and a log of every held-back input.

v2.0.0 is a full rewrite:
- a quick tap made while a press is held back keeps its length (the release fires as long after the press as you held it)
- randomize picks evenly between the safe delays near the smallest one
- platformer levels are left alone
- restarting drops anything still held back; noclip no longer cancels it
- switching the buffer off fires what's held back, release included
