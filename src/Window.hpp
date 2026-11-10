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

// How a log line is coloured.
enum Tone { Plain = 0, Late = 1, Early = 2, Bad = 3 };
void note(std::string const& line, int tone = Plain);
void saved(Save const& save);
void attempt(int number, bool newLevel);
void death(int percent);
void accuracy(int checks, int hits); // self-check: predictions compared / right

// The coach's numbers for this level.
struct Coach {
    int early = 0;        // deaths from a click / release that came too early
    double earlyMs = 0.0; // ... and by how much in all
    int late = 0;
    double lateMs = 0.0;
    int spot = -1;        // the level % you die at most (-1 = none yet)
    int spotCount = 0;
    int calls = 0;        // times the ghost said what happens next and it could be checked
    int callHits = 0;     // ... and was right
};
void coach(Coach const& stats);

} // namespace pb::window
