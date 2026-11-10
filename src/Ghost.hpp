#pragma once

#include <Geode/Geode.hpp>

#include <vector>

class PlayLayer;
class PlayerObject;
class GameObject;

// Hidden copies of the player that run ahead to see what an input would do.
namespace pb::ghost {

// `delay` / Input::at for an input that goes in right now.
constexpr double NOW = -1.0;

// One input of a run, timed in steps after the rest of the current step: the
// whole part is the step it lands in, the fraction how far into that step.
struct Input {
    double at = NOW;
    bool down = true;
};

struct Setup {
    PlayerObject* p1 = nullptr; // who to copy (the real player, or a snapshot of it)
    PlayerObject* p2 = nullptr; // set in dual mode only
    float dt = 0.25f;           // step size (one 240 TPS tick = 0.25)
    bool flying = false;        // wave / ufo / swing: a landing never counts as "safe"
    bool fresh = true;          // clear what the last run left on the ghost
    // Part of the current step still to run when the input arrived (0..1). Click
    // Between Frames splits a step at your click, so it can arrive mid-step.
    float remainder = 0.f;
    // The button as the game has it until the first input.
    bool heldBefore = false;
    // For a press: let go this many steps after it (your usual tap in this mode).
    // 0 = keep holding for the whole run.
    int tapSteps = 0;
    // Where player 1 is at the end of each step once counting started (up to recordMax).
    std::vector<cocos2d::CCPoint>* record = nullptr;
    int recordMax = 0;
    // Replays of the past: how many steps ago the copied state was (moving objects
    // are put back that far). 0 for a run that starts now.
    double stepsAgo = 0.0;
};

// What the last run did that a prediction could get wrong (for the self-check).
struct RunInfo {
    bool pad = false;   // used a pad, speed or gravity portal
    bool orb = false;   // used an orb
    bool moved = false; // moved objects that were moving
};
RunInfo lastRun();

struct Result {
    int survived = 0;             // steps it lived once counting started
    bool diedBeforeInput = false; // died while waiting for the input
    bool hitStop = false;         // reached something it can't use (orb, mode portal...)
    bool cleared = false;         // every player jumped and landed safely before the end
    bool died = false;
    int deathStep = -1;           // whole step it died in (-1 = the rest of the current step)
};

void create(PlayLayer* layer, bool withSnapshots);
void destroy(bool layerAlive);
bool ready();

// Runs the ghost(s) like the real game will: first the rest of the current step
// (Setup::remainder), then whole steps. The input goes in at `delay` (see Input),
// or right away for NOW. Then it keeps going for up to `horizon` more steps.
Result run(PlayLayer* layer, Setup const& setup, double delay, bool down, int horizon);
// No input at all: keeps the button as it is (Setup::heldBefore) for `horizon` steps.
Result coast(PlayLayer* layer, Setup const& setup, int horizon);
// Several inputs (sorted by time), counted from the start: lives `steps` steps or not.
Result replay(PlayLayer* layer, Setup const& setup, Input const* inputs, int count, int steps);

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
// can't: the run then stops there as "can't tell".
bool usePad(PlayerObject* ghost, GameObject* pad);
bool useSpeedPortal(PlayerObject* ghost, GameObject* portal, float speed);
bool useGravityPortal(PlayerObject* ghost, GameObject* portal);
// The real player used a pad: how hard the game bumped it (checked against the ghost's own value).
void sawBump(PlayerObject* player, int objectType, float bumpMod);

// Orbs (yellow, pink, red, blue, green, black): the ghost uses one when it touches
// it holding a click it hasn't used yet, the way the real player did with that orb
// type in the same mode / size / gravity. Unsupported = can't (yet).
enum class OrbUse { Used, Ignored, Unsupported };
OrbUse useOrb(PlayerObject* ghost, GameObject* orb);
// The real player used an orb: `before` its velocity and gravity.
void learnOrb(PlayerObject* player, GameObject* orb, double velocityBefore, bool flippedBefore);

// Moving objects: the real collision checks report the objects around the
// player each step; the ghost moves the ones that are moving along with it.
void track(GameObject* object, double clock);
void clearTracks();

// Snapshots of player 1, taken every few steps, so the last moments before a
// death can be played again with the click at a different time (the coach).
struct Snapshot {
    PlayerObject* player = nullptr;
    long long step = 0;  // taken at the start of this step
    bool held = false;   // the button as the game had it
    size_t inputs = 0;   // inputs logged before it
    float dt = 0.25f;
};
bool snapshotsReady();
void takeSnapshot(PlayerObject* real, long long step, bool held, size_t inputs, float dt);
// The newest snapshot taken at or before `step` (null = none).
Snapshot const* snapshotBefore(long long step);
void clearSnapshots();

} // namespace pb::ghost
