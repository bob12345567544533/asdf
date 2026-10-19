# v1.6.0
- Fixed PBs not saving with the buffer on. The hidden prediction runs no longer go through GD's collision code for blocks and spikes (which made GD drop the PB); they use their own math instead.
- Near slopes the buffer now lets inputs through instead of predicting.
- Removed the PB test switch and debug log lines.

# v1.4.9
- Fixed PBs not saving: during ghost runs the death hook swallowed GD's anti-cheat call for the real player. Now only ghost deaths are caught, like the original mod.

# v1.4.8
- PB fix: the ghost simulation now mirrors the original Predictive Input Buffer exactly (same setup, same copied fields, same step order) and no longer hooks gravity flips.

# v1.4.6
- PB fix attempt: GD's anti-cheat and progress flags are snapshotted before every ghost run and restored after.
- Ghosts collide with collision blocks again, like the original mod.

# v1.4.5
- PB fix attempt: removed the v1.4.0 hooks on GD's event/spawn/toggle functions (the original mod never had them).

# v1.4.3
- Fixed PBs being thrown away when leaving the level: ghost runs could change the jump and click counters GD checks before saving progress. They are now restored after every ghost run.

# v1.4.1
- Fixed PBs not saving: the death hook runs in its normal order again (v1.4.0 forced it to run before other mods').

# v1.4.0
- Every theme shows the time on each log line.
- 20 window themes: Clean, Terminal, Compact HUD, Neon, Retro GD, Two-pane, Analytics, Big Numbers, Slate, Minimal, Paper (light), Nord, Vaporwave, Sunset, Matrix, Amber CRT, Dracula, Ocean, Noir Gold and Midnight HUD. Switch from the button in the window's title bar (hover a name for a live preview) or the new Window Theme setting.
- Two-pane shows each attempt with a mini level bar and the % you died at. Analytics shows where in the level the buffer saves you the most.
- The window no longer takes keyboard focus from the game when clicked, comes back on top with F7, stays sharp on every monitor and never gets bigger than the screen.
- Min Gain now counts from when the delayed input goes in, so a delay that only postpones a death no longer counts as a save. Same for reaching a portal or pad before the delayed input fires.
- Randomize keeps the same 1 tick safety margin as the normal pick.
- Ghost runs can no longer set off Event triggers or spawn groups in the real level.
- Speed portals stop the simulation instead of being run past at the old speed.
- Step size tracking resets on respawn and time warp changes.
- Turning the buffer off from the window or Geode settings fires held inputs right away, like F6.
- In dual mode, releases are buffered when either player is in a mode where releases matter.
- Faster: simulations stop as soon as the answer is known.

# v1.3.1
- Log and timing popup mark delays picked by Randomize with "(random)".
- Wider window so log lines no longer get cut off with "...".

# v1.3.0
- New Randomize setting and slider in the F7 window: above 0, the buffer picks a random safe delay up to that many ticks above the smallest one instead of always the same one.

# v1.2.0
- The log shows the level % each buffered click happened at, in red.
- The log groups events by attempt with an "Attempt N" divider.

# v1.1.0
- The Buffer slider is now a range with two handles (min and max), e.g. 1-4 ticks. The buffer only uses delays inside that range.

# v1.0.0
- Buffer only: slider, Enabled / Releases / Ship / Timing popup toggles, window with log, F6 / F7 keys.
- The simulation stops at portals, pads, teleports and orbs (while holding) instead of ignoring them, so predictions near them stay correct.
- When several delays work, it uses one tick past the earliest for a safety margin.
- Capped simulation work per click, so a click that dies anyway can't cause a frame hitch.
