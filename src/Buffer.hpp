#pragma once

#include <Geode/Geode.hpp>

#include <array>
#include <deque>

namespace pb {

enum class Mode { Cube, Ship, Ball, Ufo, Wave, Robot, Spider, Swing };

Mode modeOf(PlayerObject* player);
char const* modeName(Mode mode);

// Global flags read by the hooks that block side effects during a simulation.
inline bool g_simulating = false;             // a ghost simulation is running right now
inline bool g_simDied = false;                // a ghost died during the current simulation
inline cocos2d::CCPoint g_prevPos[2] {};            // ghost positions before this sim step
inline bool g_simStopped = false;             // a ghost reached a portal/pad/orb the sim can't model
inline std::array<bool, 2> g_simHeld {};      // jump button state of each ghost
inline bool g_bypass = false;                 // we are replaying a held input, don't intercept it

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

struct State {
    std::array<PlayerObject*, 2> ghosts { nullptr, nullptr };
    std::array<Channel, 2> channels;
    int tick = 0;              // physics steps of player 1 seen this attempt
    float lastStepDt = 0.25f;  // size of a full physics step (dt passed to player 1)
    std::array<float, 16> recentDts {};
    size_t recentDtIndex = 0;
    float lastWarp = 0.f;      // time warp the step sizes above were measured at
};

inline State g;

bool isGhost(PlayerObject* player);
int ghostIndex(PlayerObject* player); // 0 / 1, or -1 if not a ghost
void setGhosts(PlayerObject* a, PlayerObject* b);
void clearGhosts(PlayerObject* a, PlayerObject* b);

// Drops every held input without firing it (death, restart, quitting).
void clearQueues();

// Starts tick counting and step-size tracking over (new level or attempt).
void resetTiming();

// Real milliseconds per physics step right now.
double tickMs(PlayLayer* layer);

// Called from GJBaseGameLayer::handleButton. Returns true if the input was taken
// and will be fired later instead.
bool onButton(PlayLayer* layer, bool down, int button, bool isPlayer1);

// Called right before player 1 runs a physics step. Fires inputs that are due.
void onStep(PlayLayer* layer, float dt);

// Fires everything that is queued on a channel right now, in order.
void flushChannel(PlayLayer* layer, int channel);

// True for objects that change the player in ways the simulation can't follow
// (portals, pads, teleports, orbs). The simulation stops when it reaches one.
bool stopsSimulation(GameObject* object, PlayerObject* ghost);

} // namespace pb
