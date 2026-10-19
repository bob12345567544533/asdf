#pragma once

#include <string>

namespace pb::menu {

// Shows or hides the settings window. Call from the main thread.
// On Windows it's a separate window (handy on a second monitor) that's hidden
// from screen capture. Elsewhere it opens the normal Geode settings popup.
void toggle();

// Shows the window if it isn't open yet (used for "open on launch").
void open();

// Pushes the current settings to the window (after they change elsewhere).
void sync();

// Adds a line to the window's live log. Call from the main thread.
// `percent` is the level % it happened at (shown in red), -1 to leave it out.
void log(std::string const& line, int percent = -1);

// Starts a new attempt section in the log ("-- Attempt 12 --").
void attempt(int number);

} // namespace pb::menu
