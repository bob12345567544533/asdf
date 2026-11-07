#pragma once

#include <Geode/Geode.hpp>

class PlayLayer;
class PlayerObject;
class GameObject;

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
    int survived = 0;             // steps it lived after the input went in
    bool diedBeforeInput = false; // died while waiting for the input
    bool hitStop = false;         // reached something it can't use (orb, mode portal...)
    bool cleared = false;         // every player jumped and landed safely before the end
};

void create(PlayLayer* layer);
void destroy();
bool ready();

// Runs the ghost(s) with the button as it is now until `delay` steps have gone
// by (a fraction means the input lands that far into a step), then applies
// `down` and keeps going for up to `horizon` more steps.
Result run(PlayLayer* layer, Setup const& setup, double delay, bool down, int horizon);

// For the hooks that keep a ghost run from touching the real game.
bool running();
bool isGhost(PlayerObject* player);
void markDead();
bool held(PlayerObject* ghost);
bool stopped();
void markStopped();

// Pads, speed and gravity portals the ghost can use itself. Returns false when it
// can't (yet): the run then stops there like before.
bool usePad(PlayerObject* ghost, GameObject* pad);
bool useSpeedPortal(PlayerObject* ghost, GameObject* portal, float speed);
bool useGravityPortal(PlayerObject* ghost, GameObject* portal);
// The real player used a pad: remember how hard GD bumps for that pad type.
void learnBump(int objectType, float bumpMod);

// Moving objects: the real collision checks report the objects around the
// player each step; the ghost moves the ones that are moving along with it.
void track(GameObject* object, double clock);
void clearTracks();

} // namespace pb::ghost
