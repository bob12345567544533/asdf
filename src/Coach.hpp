#pragma once

class PlayLayer;

// The coach: after a death it plays your last moments again with the click a
// little earlier / later, and tells you how far off the timing was. It only
// ever writes lines to the window; nothing here changes an input.
namespace pb::coach {

void levelStarted(PlayLayer* layer);
void levelEnded();
// Death / restart: wrap up whatever is still being worked out, forget the inputs.
void reset();

// An input reached the game at `time` (in steps). `moved`: ticks the buffer held it back.
void logInput(double time, bool down, float moved);
size_t inputCount();

// An input was let through although the ghost says it dies: `needed` is the delay
// that would have saved it (-1 = none found), `limit` the most the buffer may add.
void missed(long long step, bool down, int needed, int limit);

// Player 1 really died. Nothing is simulated here (it's called from inside the
// game's collision check): the work happens in tick().
struct Death {
    long long step = 0;
    int percent = 0;
    int mode = 0;
    bool held = false; // the button as the game had it
    bool dual = false;
    int tap = 0;       // your usual tap in this mode (steps), 0 = unknown
    int limit = 0;     // the buffer's limit in this mode
};
void died(PlayLayer* layer, Death const& death);
// An input arrived after the death (a click that came too late to count).
void lateInput(bool down);

// Once a frame, between physics steps: a slice of the work.
void tick(PlayLayer* layer, float dt);
// True while a death is being worked out (snapshots stay as they are).
bool busy();

// The ghost said what would happen after an input, and then it did / didn't.
void called(bool right);

} // namespace pb::coach
