#pragma once

namespace pb::menu {

// Opens or closes the quick settings window. Call from the main thread.
// On Windows this is a separate window hidden from screen capture.
// Elsewhere it opens the normal Geode settings popup.
void toggle();

} // namespace pb::menu
