#pragma once

#include <string>

namespace pb::menu {

// Shows or hides the settings window. Call from the main thread.
// On Windows it's a separate window (handy on a second monitor) that's hidden
// from screen capture. Elsewhere it opens the normal Geode settings popup.
void toggle();

// Shows the window if it isn't open yet (used for "open on launch").
void open();

// Creates the (hidden) window in the background so the log fills up even
// before it's first shown.
void prepare();

// Pushes the current settings to the window (after they change elsewhere).
void sync();

// Adds a plain line to the window's log ("Buffer ON"). Main thread.
void log(std::string const& line);

// Logs a buffered input. `mode` is the Mode enum value (0 = cube .. 7 = swing),
// `percent` the level % it happened at (-1 if unknown). Main thread.
void saved(int ticks, double ms, int mode, bool release, bool random, int percent);

// Starts a new attempt in the log. `newLevel` resets the per-level stats. Main thread.
void attempt(int number, bool newLevel = false);

// The current attempt ended in a death at this level %. Main thread.
void death(int percent);

} // namespace pb::menu
