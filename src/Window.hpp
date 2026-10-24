#pragma once

#include <string>

// The settings + log window (F7). On Windows it's its own window (handy on a
// second monitor) that never takes focus from the game; elsewhere F7 opens the
// normal Geode settings. Everything here is called from the main thread.
namespace pb::window {

struct Save {
    int ticks = 0;
    double ms = 0.0;
    int mode = 0;        // 0 cube, 1 ship, 2 ball, 3 ufo, 4 wave, 5 robot, 6 spider, 7 swing
    bool release = false;
    bool random = false;
    int percent = -1;    // level % it happened at
};

void toggle();
void open();     // show if not shown yet ("open on launch")
void prepare();  // create it hidden so the log fills up before it's first shown
void sync();     // settings changed elsewhere: show the new values

void note(std::string const& line);
void saved(Save const& save);
void attempt(int number, bool newLevel);
void death(int percent);

} // namespace pb::window
