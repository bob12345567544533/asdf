#pragma once

// Display only: the Minimal window (F7) and the in-game timing popup.
// Nothing here changes how the buffer works.

class PlayLayer;

namespace ui {

// Starts the window (Windows). Call once on load.
void init();

// A new level started: put the popup layer on it.
void attach(PlayLayer* layer);

// An input was buffered by `ticks` steps. Main thread.
void saved(int ticks, bool press, int limit);

// F6 changed the stage (0..3 = stages 1-4, 4 = off). Main thread.
void stage(int index, int all, int wave);

} // namespace ui
