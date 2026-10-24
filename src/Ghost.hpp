#pragma once

class PlayLayer;
class PlayerObject;

// Hidden copies of the player that run ahead to see what an input would do.
namespace pb::ghost {

struct Setup {
    PlayerObject* p1 = nullptr;
    PlayerObject* p2 = nullptr; // set in dual mode only
    float dt = 0.25f;           // step size (one 240 TPS tick = 0.25)
    bool flying = false;        // wave / ufo / swing: a landing never counts as "safe"
    bool fresh = true;          // clear what the last check left on the ghost
};

struct Result {
    int survived = 0;            // steps it lived after the input went in
    bool diedBeforeInput = false; // died while waiting for the input
    bool hitStop = false;        // reached a portal / pad / orb it can't use
    bool cleared = false;        // jumped and landed safely before the end
};

void create(PlayLayer* layer);
void destroy();
bool ready();

// Runs the ghost(s) `delay` steps with the button as it is now, then applies
// `down` and keeps going for up to `horizon` more steps.
Result run(PlayLayer* layer, Setup const& setup, int delay, bool down, int horizon);

// For the hooks that keep a ghost run from touching the real game.
bool running();
bool isGhost(PlayerObject* player);
void markDead();
bool held(PlayerObject* ghost);
bool stopped();
void markStopped();

} // namespace pb::ghost
