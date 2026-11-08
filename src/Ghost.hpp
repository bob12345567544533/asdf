#pragma once

#include <Geode/Geode.hpp>

class PlayLayer;
class PlayerObject;
class GameObject;

// Hidden copies of the player that run ahead to see what an input would do.
namespace pb::ghost {

// `delay` for an input that goes in right now (passed straight through to GD).
constexpr double NOW = -1.0;

struct Setup {
    PlayerObject* p1 = nullptr;
    PlayerObject* p2 = nullptr; // set in dual mode only
    float dt = 0.25f;           // step size (one 240 TPS tick = 0.25)
    bool flying = false;        // wave / ufo / swing: a landing never counts as "safe"
    bool fresh = true;          // clear what the last check left on the ghost
    // Part of the current step still to run when the input arrived (0..1). Click
    // Between Frames splits a step at your click, so it can arrive mid-step.
    float remainder = 0.f;
    // For a press: let go this many steps after it (your usual tap in this mode).
    // 0 = keep holding for the whole check.
    int tapSteps = 0;
    // NOW with down = true while the button is already held: keep holding (no new click).
    bool keepHolding = false;
    // Self-check: where player 1 is at the end of each step after the input (up to recordMax).
    std::vector<cocos2d::CCPoint>* record = nullptr;
    int recordMax = 0;
};

// What the last run did that a prediction could get wrong (for the self-check).
struct RunInfo {
    bool pad = false;   // used a pad, speed or gravity portal
    bool orb = false;   // used an orb
    bool moved = false; // moved objects that were moving
};
RunInfo lastRun();

struct Result {
    int survived = 0;             // steps it lived after the input went in
    bool diedBeforeInput = false; // died while waiting for the input
    bool hitStop = false;         // reached something it can't use (orb, mode portal...)
    bool cleared = false;         // every player jumped and landed safely before the end
};

void create(PlayLayer* layer);
void destroy();
bool ready();

// Runs the ghost(s) like the real game will: first the rest of the current step
// (Setup::remainder), then whole steps. The input goes in at the start of step
// `delay` counted after that (a fraction: that far into the step, split the way
// Click Between Frames splits it), or right away for NOW. Then it keeps going
// for up to `horizon` more steps.
Result run(PlayLayer* layer, Setup const& setup, double delay, bool down, int horizon);

// Whether a step can be split for an input mid-step (same rule as Click Between
// Frames: on the ground, touching an orb, dashing, or a flying mode).
bool splittable(PlayerObject* player);
// The middle of a split step: collisions so far (no damage) and a fresh collision log.
void splitCollide(PlayLayer* layer, PlayerObject* player, bool startedOnGround, float dt);

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

// Orbs (yellow, pink, red, blue, green, black): the ghost uses one when it touches
// it holding a click it hasn't used yet, the way the real player did with that orb
// type in the same mode / size / gravity. Returns false when it can't (yet).
enum class OrbUse { Used, Ignored, Unsupported };
OrbUse useOrb(PlayerObject* ghost, GameObject* orb);
// The real player used an orb: `before` / `after` its velocity and gravity.
void learnOrb(PlayerObject* player, GameObject* orb, double velocityBefore, bool flippedBefore);

// Moving objects: the real collision checks report the objects around the
// player each step; the ghost moves the ones that are moving along with it.
void track(GameObject* object, double clock);
void clearTracks();

} // namespace pb::ghost
