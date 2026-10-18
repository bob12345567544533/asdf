# Predictive Input Buffer

A **practice tool** that forgives clicks that are a few milliseconds too early.

Every time you click, the mod quietly runs the next few hundred milliseconds of gameplay with an invisible copy of your player. If your click would kill you, but the same click a little later wouldn't, it holds the click and fires it at the right moment. If your click was fine, or nothing would have saved you, it goes through untouched.

## Features
- **Per-gamemode limits** in milliseconds (cube, ship, ball, UFO, wave, robot, spider, swing). 0 turns a mode off.
- **Release buffering** for wave, ship and robot, where letting go too early kills you.
- **Timing feedback**: shows how early each saved click was, so you can learn the timing instead of relying on the buffer.
- **Toggle key** (F6 by default) to turn it on/off in a level.
- Works with **dual mode** and **2-player mode** (each player is handled on its own).
- Uses real time, so it stays consistent if you change TPS or a level uses time warp.

## Notes
- This changes your inputs. Treat it like any other practice hack: runs with it on don't count.
- The ghost only checks blocks, slopes and spikes. It ignores orbs, pads and portals (touching them from a simulation would mess with the real level), so it's less useful right around those.
