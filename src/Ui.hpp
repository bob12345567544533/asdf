#pragma once

#include <string>

class PlayLayer;

// In-game timing popup (display only).
namespace ui {

// A new level started: put the popup layer on it.
void attach(PlayLayer* layer);

// Shows a short line bottom left that fades out.
void popup(std::string const& text, bool good = true);

} // namespace ui
