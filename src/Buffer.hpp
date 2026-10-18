#pragma once

#include <Geode/Geode.hpp>

#include <array>
#include <deque>
#include <span>

namespace pb {

enum class Mode { Cube, Ship, Ball, Ufo, Wave, Robot, Spider, Swing };

Mode modeOf(PlayerObject* player);
char const* modeName(Mode mode);
bool isFlyingMode(Mode mode);

// Global flags read by the hooks that block side effects during a simulation.
inline bool g_simulating = false; // a ghost simulation is running right now
inline bool g_simDied = false;    // a ghost died during the current simulation
inline bool g_bypass = false;     // we are replaying a held input, don't intercept it

// Settings read every physics step, cached so we don't look them up 240 times a
// second. All zeros when the mod is turned off.
struct Config {
    float shipAssist = 0.f;   // 0..0.5, share of vertical acceleration removed
    float ufoAssist = 0.f;
    float robotAssist = 0.f;
    int ledgeTicks = 0;       // ledge grace window (240 TPS ticks)
    int orbTicks = 0;         // late orb window (240 TPS ticks)
    float hitboxShrink = 0.f; // 0..0.5, how much smaller your hazard hitbox is
    int wavePixels = 0;       // max wave nudge distance
    float slowmo = 0.f;       // 0..0.5, how much the game slows down near danger
};
inline Config g_cfg;
void refreshCachedSettings();

// An input we are holding back, plus the physics step it should fire on.
struct QueuedInput {
    bool down = false;
    bool isPlayer1 = true;
    int fireAt = 0;
};

// Channel 0 drives player 1 (and player 2 in dual). Channel 1 is player 2 in 2-player mode.
struct Channel {
    std::deque<QueuedInput> queue;
    int shift = 0; // how many steps the inputs in the queue are being pushed back
};

struct Stats {
    int attemptCount = 0;
    int sessionCount = 0;
    double sessionEarlyMs = 0.0;
};

// What we remember about each real player between steps (0 = player 1, 1 = player 2).
struct PlayerTrack {
    bool held = false;            // jump button as the game sees it
    bool prevOnGround = false;
    int leftGroundTick = -1000;   // step the player last left the ground
    bool leftByJump = true;       // left by jumping (no ledge grace) or by running off
    RingObject* ring = nullptr;   // last orb touched and not used
    int ringTick = -1000;
    int lastNudgeTick = -1000;
};

struct State {
    std::array<PlayerObject*, 2> ghosts { nullptr, nullptr };
    std::array<Channel, 2> channels;
    std::array<PlayerTrack, 2> track;
    int tick = 0;              // physics steps of player 1 seen this attempt
    float lastStepDt = 0.25f;  // size of a full physics step (dt passed to player 1)
    std::array<float, 16> recentDts {};
    size_t recentDtIndex = 0;
    float slowFactor = 1.f;    // game speed for the next frame (danger slow-mo)
    Stats stats;
};

inline State g;

bool isGhost(PlayerObject* player);
void setGhosts(PlayerObject* a, PlayerObject* b);
void clearGhosts(PlayerObject* a, PlayerObject* b);

// Drops every held input without firing it (death, restart, quitting).
void clearQueues();

// Real milliseconds per physics step right now.
double tickMs(PlayLayer* layer);

// Called from GJBaseGameLayer::handleButton. Returns true if the input was taken
// and will be fired later instead.
bool onButton(PlayLayer* layer, bool down, int button, bool isPlayer1);

// Called right before player 1 runs a physics step. Fires inputs that are due.
void onStep(PlayLayer* layer, float dt);

// Fires everything that is queued on a channel right now, in order.
void flushChannel(PlayLayer* layer, int channel);

// --- Assists ---------------------------------------------------------------

// Clears per-attempt tracking (restart, new level).
void resetTracks();

// Remembers the jump button state the game ends up with.
void noteHeld(PlayLayer* layer, bool down, int button, bool isPlayer1);

// Called before each real physics step of player 1 or 2 (ledge tracking).
void beforeRealUpdate(PlayLayer* layer, PlayerObject* player);

// Share of vertical acceleration to remove for this player right now (0 = none).
float assistFor(PlayLayer* layer, PlayerObject* player);

// Orb tracking for late orb clicks.
void noteRingTouched(PlayLayer* layer, PlayerObject* player, RingObject* ring);
void noteRingUsed(PlayLayer* layer, PlayerObject* player);

// True if a hazard hit should be ignored because it only grazed the player.
bool hitboxForgives(PlayLayer* layer, PlayerObject* player, GameObject* object);

// Runs after each frame: wave nudges and danger slow-mo.
void onFrameEnd(PlayLayer* layer);

} // namespace pb
