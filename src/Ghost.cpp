#include "Ghost.hpp"

#include "Config.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace pb::ghost {

namespace {

PlayerObject* g_ghost[2] { nullptr, nullptr };
bool g_running = false;
bool g_dead = false;
bool g_stopped = false;
bool g_held[2] { false, false };
bool g_ready[2] { false, false }; // pressed since the last orb it used (a click an orb can still take)
std::vector<GameObject*> g_used; // pads / portals / orbs the ghost already used this run
RunInfo g_info;

int modeKey(PlayerObject* p) {
    if (p->m_isShip) return 1;
    if (p->m_isBall) return 2;
    if (p->m_isBird) return 3;
    if (p->m_isDart) return 4;
    if (p->m_isRobot) return 5;
    if (p->m_isSpider) return 6;
    if (p->m_isSwing) return 7;
    return 0;
}

// ---------------------------------------------------------------------------
// Pads
// ---------------------------------------------------------------------------

// How hard a pad bumps the player: the game's own table (yellow 1, pink and red
// by gamemode / size).
float bumpFor(PlayerObject* p, GameObjectType type) {
    bool mini = p->m_vehicleSize < 1.f;
    if (type == GameObjectType::PinkJumpPad) {
        if (p->m_isShip) return 0.35f;
        if (p->m_isBird) return 0.4f;
        if (p->m_isBall || p->m_isSpider) return 0.7f;
        return 0.65f;
    }
    if (type == GameObjectType::RedJumpPad) {
        if (p->m_isShip) return mini ? 0.95f : 0.63f;
        if (p->m_isBird) return mini ? 0.98f : 0.6f;
        return 1.25f;
    }
    return 1.f;
}

// ... and what the game really used for the real player this session (wins over
// the table if they ever differ).
std::unordered_map<int, float> g_bumpSeen;

int bumpKey(PlayerObject* p, int objectType) {
    return objectType * 64 + modeKey(p) * 2 + (p->m_vehicleSize < 1.f ? 1 : 0);
}

bool jumpPad(int objectType) {
    switch (static_cast<GameObjectType>(objectType)) {
        case GameObjectType::YellowJumpPad:
        case GameObjectType::PinkJumpPad:
        case GameObjectType::RedJumpPad:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Orbs
// ---------------------------------------------------------------------------

// What each orb does to the player, learned from the real player: the velocity it
// leaves you with and whether it flips gravity, per orb type / mode / size / gravity.
struct OrbEffect {
    double velocity = 0.0;
    bool flips = false;
};
std::unordered_map<std::string, OrbEffect> g_orbs;

std::string orbKey(GameObject* orb, PlayerObject* p, bool upsideDown) {
    return fmt::format("orb-{}-{}-{}-{}", static_cast<int>(orb->m_objectType), modeKey(p), p->m_vehicleSize < 1.f ? 1 : 0,
                       upsideDown ? 1 : 0);
}

bool supportedOrb(GameObject* orb) {
    switch (orb->m_objectType) {
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::DropRing:
            return true;
        default:
            return false;
    }
}

// Learned orbs are saved as plain values ("<key>-v" velocity, "<key>-f" flips),
// looked up the first time an orb type / mode comes up.
OrbEffect const* findOrb(std::string const& key) {
    auto it = g_orbs.find(key);
    if (it != g_orbs.end()) return &it->second;
    auto mod = Mod::get();
    if (!mod->hasSavedValue(key + "-v")) return nullptr;
    OrbEffect e;
    e.velocity = mod->getSavedValue<double>(key + "-v");
    e.flips = mod->getSavedValue<bool>(key + "-f");
    return &(g_orbs[key] = e);
}

// ---------------------------------------------------------------------------
// Moving objects
// ---------------------------------------------------------------------------

// Objects around the player and how fast they move (units per unit of step time).
struct Track {
    double x = 0.0;
    double y = 0.0;
    double vx = 0.0;
    double vy = 0.0;
    double clock = -1.0;
};
std::unordered_map<GameObject*, Track> g_tracks;
double g_lastClock = 0.0;

// Moves the objects that are moving to where they'll be during a ghost step, and
// puts every one of them back exactly as it was when the run ends.
class Movers {
public:
    struct Moved {
        GameObject* object;
        CCPoint node;
        double x;
        double y;
        CCPoint last;
        double vx;
        double vy;
    };

    Movers(bool on, float dt) : m_dt(dt) {
        if (!on || g_tracks.empty()) return;
        for (auto& [object, t] : g_tracks) {
            if (!object || t.clock < g_lastClock - 1.0) continue; // not seen around the player lately
            if (std::abs(t.vx) < 1e-4 && std::abs(t.vy) < 1e-4) continue;
            m_moved.push_back({ object, object->getPosition(), object->m_positionX, object->m_positionY,
                                object->m_lastPosition, t.vx, t.vy });
        }
    }
    ~Movers() { restore(); }

    // Positions `steps` from now (negative = in the past).
    void place(double steps) {
        double t = steps * static_cast<double>(m_dt);
        for (auto& m : m_moved) {
            m.object->setPosition({ static_cast<float>(m.node.x + m.vx * t), static_cast<float>(m.node.y + m.vy * t) });
        }
    }

    void restore() {
        for (auto& m : m_moved) {
            m.object->setPosition(m.node);
            m.object->m_positionX = m.x;
            m.object->m_positionY = m.y;
            m.object->m_lastPosition = m.last;
            m.object->setObjectRectDirty(true);
            m.object->setOrientedRectDirty(true);
        }
        m_moved.clear();
    }

    bool any() const { return !m_moved.empty(); }

private:
    float m_dt;
    std::vector<Moved> m_moved;
};

// ---------------------------------------------------------------------------
// Copying the player
// ---------------------------------------------------------------------------

// Fields copyAttributes leaves out (GD 2.2081 Windows; same set the original
// mod copied): slope / ground state and a few movement values.
template <class T>
void copyRaw(PlayerObject* to, PlayerObject* from, size_t offset) {
    std::memcpy(reinterpret_cast<char*>(to) + offset, reinterpret_cast<char const*>(from) + offset, sizeof(T));
}

void mirror(PlayerObject* to, PlayerObject* from) {
    to->setVisible(false);
    to->copyAttributes(from);
    to->setScale(from->getScale());
    to->setContentSize(from->getContentSize());
    copyRaw<float>(to, from, 0x2fc);
    copyRaw<float>(to, from, 0x300);
    copyRaw<float>(to, from, 0xb84);
    copyRaw<bool>(to, from, 0x9c1);
    copyRaw<bool>(to, from, 0x9bf);
    copyRaw<float>(to, from, 0x9f0);
    copyRaw<float>(to, from, 0x9f4);
}

void press(int i, bool down) {
    if (down) g_ghost[i]->pushButton(PlayerButton::Jump);
    else g_ghost[i]->releaseButton(PlayerButton::Jump);
    if (down && !g_held[i]) g_ready[i] = true;
    g_held[i] = down;
}

// Only plain resets: GD's own reset functions can touch game-wide state (the cursor).
void freshen(PlayerObject* ghost) {
    ghost->releaseButton(PlayerButton::Jump);
    ghost->m_isDead = false;
}

void clearLogs(PlayerObject* ghost) {
    ghost->m_collisionLogTop->removeAllObjects();
    ghost->m_collisionLogBottom->removeAllObjects();
    ghost->m_collisionLogLeft->removeAllObjects();
    ghost->m_collisionLogRight->removeAllObjects();
}

PlayerObject* make(PlayLayer* layer) {
    auto ghost = PlayerObject::create(1, 1, layer, layer, true);
    if (!ghost) return nullptr;
    ghost->retain();
    ghost->setPosition({ 0.f, 105.f });
    ghost->setVisible(false);
    layer->m_objectLayer->addChild(ghost);
    return ghost;
}

bool used(GameObject* object) {
    return std::find(g_used.begin(), g_used.end(), object) != g_used.end();
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

constexpr int SNAPSHOTS = 8;
// Never part of the scene: the snapshot players (and the particles they come
// with) are only ever copied to and from, so they cost nothing while you play.
CCLayer* g_shelf = nullptr;
Snapshot g_snaps[SNAPSHOTS];
bool g_snapValid[SNAPSHOTS] {};
int g_snapNext = 0;

void makeSnapshots(PlayLayer* layer) {
    g_shelf = CCLayer::create();
    if (!g_shelf) return;
    g_shelf->retain();
    for (int i = 0; i < SNAPSHOTS; ++i) {
        auto holder = PlayerObject::create(1, 1, layer, g_shelf, true);
        if (!holder) {
            // all or nothing
            for (auto& s : g_snaps) s = {};
            g_shelf->release();
            g_shelf = nullptr;
            return;
        }
        holder->setVisible(false);
        g_shelf->addChild(holder);
        g_snaps[i] = {};
        g_snaps[i].player = holder;
        g_snapValid[i] = false;
    }
    g_snapNext = 0;
}

// ---------------------------------------------------------------------------
// One run
// ---------------------------------------------------------------------------

// Nothing a ghost started may keep running once its run is over: animations it set
// off (rotations, squish, robot / spider sprites) and particles would otherwise pile
// up run after run, and the game would get slower and slower.
void stopActions(CCNode* node, int depth = 0) {
    if (!node || depth > 6) return;
    node->stopAllActions();
    if (auto kids = node->getChildren()) {
        for (auto kid : CCArrayExt<CCNode*>(kids)) stopActions(kid, depth + 1);
    }
}

void quiet(PlayerObject* p) {
    if (!p) return;
    stopActions(p);
    for (auto ps : { p->m_playerGroundParticles, p->m_trailingParticles, p->m_shipClickParticles, p->m_vehicleGroundParticles,
                     p->m_ufoClickParticles, p->m_robotBurstParticles, p->m_dashParticles, p->m_swingBurstParticles1,
                     p->m_swingBurstParticles2, p->m_landParticles0, p->m_landParticles1 }) {
        if (ps && ps->isActive()) ps->stopSystem();
    }
}

struct RunGuard {
    RunGuard() {
        g_running = true;
        g_dead = false;
        g_stopped = false;
        g_held[0] = g_held[1] = false;
        g_ready[0] = g_ready[1] = false;
        g_used.clear();
        g_info = {};
    }
    ~RunGuard() {
        g_running = false;
        quiet(g_ghost[0]);
        quiet(g_ghost[1]);
    }
};

// `judge`: the input after which steps are counted (-1 = from the very start).
// `landing`: cube-like modes count a jump that lands and stays down as safe.
Result simulate(PlayLayer* layer, Setup const& s, Input const* inputs, int count, int judge, int horizon, bool landing) {
    Result result;
    if (!layer || !s.p1 || !g_ghost[0]) {
        result.survived = horizon;
        return result;
    }
    bool dual = s.p2 && g_ghost[1];
    int players = dual ? 2 : 1;
    PlayerObject* from[2] { s.p1, s.p2 };
    float rem = std::clamp(s.remainder, 0.f, 0.999f);
    if (rem < 0.01f) rem = 0.f;

    RunGuard guard;
    Movers movers(config().movingObjects, s.dt);
    g_info.moved = movers.any();

    int next = 0; // the next input to go in
    bool applied = judge < 0;
    bool judgeDown = judge >= 0 && judge < count && inputs[judge].down;
    bool startsNow = count > 0 && inputs[0].at < 0.0;
    for (int i = 0; i < players; ++i) {
        if (s.fresh) freshen(g_ghost[i]);
        mirror(g_ghost[i], from[i]);
        if (startsNow) {
            press(i, inputs[0].down);
        }
        else {
            press(i, s.heldBefore); // the button as it is now, until the first input
            g_ready[i] = false;     // a hold from before the run isn't a click an orb can take
        }
    }
    if (startsNow) {
        if (judge == 0) applied = true;
        next = 1;
    }

    double t = -s.stepsAgo; // steps from now (for moving objects)

    // per player: a jump that lands and stays down
    int grounded[2] {};
    int airborne[2] {};
    bool clear[2] {};
    int sincePress = 0;
    auto counted = [&]() -> bool { // one more step lived; true = the run is over
        result.survived++;
        if (s.record && static_cast<int>(s.record->size()) < s.recordMax) s.record->push_back(g_ghost[0]->getPosition());
        // your usual tap: let go when you normally would
        if (judge >= 0 && judgeDown && s.tapSteps > 0 && ++sincePress == s.tapSteps) {
            for (int i = 0; i < players; ++i) press(i, false);
        }
        if (result.survived >= horizon) return true;
        if (!landing || s.flying) return false;
        bool allClear = true;
        for (int i = 0; i < players; ++i) {
            if (!clear[i]) {
                if (!g_ghost[i]->m_isOnGround) {
                    airborne[i]++;
                    grounded[i] = 0;
                }
                else if (airborne[i] >= 6 && ++grounded[i] > 2) {
                    clear[i] = true;
                }
            }
            allClear = allClear && clear[i];
        }
        if (allClear) {
            result.survived = horizon;
            result.cleared = true;
            return true;
        }
        return false;
    };
    auto death = [&](int step) {
        result.died = true;
        result.deathStep = step;
        result.diedBeforeInput = !applied;
    };
    // the end of a step: collisions, then rotation, like the game. false = the run ended
    auto finish = [&](int step, float rotation) {
        if (g_dead) {
            death(step);
            return false;
        }
        for (int i = 0; i < players; ++i) {
            clearLogs(g_ghost[i]);
            layer->checkCollisions(g_ghost[i], s.dt, false);
            if (g_dead) {
                death(step);
                return false;
            }
        }
        if (g_stopped) {
            result.hitStop = true;
            return false;
        }
        for (int i = 0; i < players; ++i) g_ghost[i]->updateRotation(rotation);
        return true;
    };

    // the rest of the step the input arrived in
    if (rem > 0.f) {
        t += rem;
        movers.place(t);
        for (int i = 0; i < players; ++i) g_ghost[i]->update(s.dt * rem);
        if (!finish(-1, s.dt * rem)) return result;
        if (applied && counted()) return result;
    }

    double lastAt = count > 0 ? std::max(0.0, inputs[count - 1].at) : 0.0;
    int cap = horizon + static_cast<int>(lastAt) + 3;
    for (int step = 0; step < cap; ++step) {
        t += 1.0;
        movers.place(t);
        // same rule as the real game: only some states let an input land mid-step
        bool canSplit = !dual && splittable(g_ghost[0]);
        bool startedOnGround = g_ghost[0]->m_isOnGround;
        CCPoint startPos = g_ghost[0]->getPosition();
        float cursor = 0.f; // how much of this step has run
        while (next < count && inputs[next].at < step + 1.0 - 1e-6) {
            double at = inputs[next].at;
            float frac = at <= step ? 0.f : static_cast<float>(at - step); // left over from an earlier step: right away
            if (frac < 0.01f) frac = 0.f;
            if (frac > cursor + 1e-4f) {
                if (!canSplit) break; // whole step now, the input at the start of the next one
                g_ghost[0]->update(s.dt * (frac - cursor));
                if (g_dead) {
                    death(step);
                    return result;
                }
                splitCollide(layer, g_ghost[0], startedOnGround, s.dt);
                cursor = frac;
            }
            for (int i = 0; i < players; ++i) press(i, inputs[next].down);
            if (next == judge) applied = true;
            ++next;
        }
        for (int i = 0; i < players; ++i) g_ghost[i]->update(s.dt * (1.f - cursor));
        if (!finish(step, s.dt)) return result;
        if (cursor > 0.f) g_ghost[0]->m_lastPosition = startPos; // as after a real split step
        if (applied && counted()) return result;
    }
    return result;
}

} // namespace

void create(PlayLayer* layer, bool withSnapshots) {
    destroy(false);
    clearTracks();
    g_bumpSeen.clear();
    if (!layer || !layer->m_objectLayer) return;
    g_ghost[0] = make(layer);
    g_ghost[1] = make(layer);
    if (withSnapshots && g_ghost[0]) makeSnapshots(layer);
}

void destroy(bool layerAlive) {
    // Just let go: the level owns the node and cleans it up with everything else
    // (taking it out by hand runs PlayerObject's exit code, best left alone).
    for (auto& ghost : g_ghost) {
        if (!ghost) continue;
        ghost->release();
        ghost = nullptr;
    }
    // The snapshot players are ours. If their level is already gone, leave them be
    // rather than destroy players whose level no longer exists.
    if (g_shelf && layerAlive) g_shelf->release();
    g_shelf = nullptr;
    for (auto& s : g_snaps) s = {};
    for (auto& v : g_snapValid) v = false;
    g_running = false;
    clearTracks();
}

bool ready() {
    return g_ghost[0] != nullptr;
}

Result run(PlayLayer* layer, Setup const& setup, double delay, bool down, int horizon) {
    Input input { delay < 0.0 ? NOW : delay, down };
    return simulate(layer, setup, &input, 1, 0, horizon, true);
}

Result coast(PlayLayer* layer, Setup const& setup, int horizon) {
    return simulate(layer, setup, nullptr, 0, -1, horizon, false);
}

Result replay(PlayLayer* layer, Setup const& setup, Input const* inputs, int count, int steps) {
    return simulate(layer, setup, inputs, count, -1, steps, false);
}

bool splittable(PlayerObject* p) {
    return p->m_isOnGround || (p->m_touchingRings && p->m_touchingRings->count()) || p->m_isDashing || p->m_isDart
        || p->m_isBird || p->m_isShip || p->m_isSwing;
}

void splitCollide(PlayLayer* layer, PlayerObject* p, bool startedOnGround, float dt) {
    // (as Click Between Frames does it) a platform moving down would otherwise drop the ground under you
    if ((p->m_yVelocity < 0) ^ p->m_isUpsideDown) p->m_isOnGround = startedOnGround;
    if (!p->m_isOnSlope || p->m_isDart) layer->checkCollisions(p, 0.f, true);
    else layer->checkCollisions(p, dt, true);
    clearLogs(p);
    p->m_lastCollisionLeft = -1;
    p->m_lastCollisionRight = -1;
    p->m_lastCollisionBottom = -1;
    p->m_lastCollisionTop = -1;
}

bool running() { return g_running; }

RunInfo lastRun() { return g_info; }

bool isGhost(PlayerObject* player) {
    return player && (player == g_ghost[0] || player == g_ghost[1]);
}

void markDead() { g_dead = true; }

bool held(PlayerObject* ghost) {
    if (ghost == g_ghost[0]) return g_held[0];
    if (ghost == g_ghost[1]) return g_held[1];
    return false;
}

bool stopped() { return g_stopped; }

void markStopped() { g_stopped = true; }

bool usePad(PlayerObject* ghost, GameObject* pad) {
    if (used(pad)) return true;
    int type = static_cast<int>(pad->m_objectType);
    if (!jumpPad(type)) return false;
    auto seen = g_bumpSeen.find(bumpKey(ghost, type));
    float bump = seen != g_bumpSeen.end() ? seen->second : bumpFor(ghost, pad->m_objectType);
    g_used.push_back(pad);
    g_info.pad = true;
    ghost->bumpPlayer(bump, type, true, pad);
    return true;
}

bool useSpeedPortal(PlayerObject* ghost, GameObject* portal, float speed) {
    if (used(portal)) return true;
    g_used.push_back(portal);
    g_info.pad = true;
    ghost->updateTimeMod(speed, true);
    return true;
}

bool useGravityPortal(PlayerObject* ghost, GameObject* portal) {
    if (used(portal)) return true;
    bool flip = ghost->m_isUpsideDown;
    switch (portal->m_objectType) {
        case GameObjectType::InverseGravityPortal: flip = true; break;
        case GameObjectType::NormalGravityPortal: flip = false; break;
        case GameObjectType::GravityTogglePortal: flip = !ghost->m_isUpsideDown; break;
        default: return false;
    }
    g_used.push_back(portal);
    g_info.pad = true;
    if (flip != ghost->m_isUpsideDown) ghost->flipGravity(flip, true);
    return true;
}

void sawBump(PlayerObject* player, int objectType, float bumpMod) {
    if (!player || !jumpPad(objectType)) return;
    g_bumpSeen[bumpKey(player, objectType)] = bumpMod;
}

OrbUse useOrb(PlayerObject* ghost, GameObject* orb) {
    if (!supportedOrb(orb)) return OrbUse::Unsupported;
    int i = ghost == g_ghost[1] ? 1 : 0;
    if (used(orb)) return OrbUse::Ignored;
    if (!g_held[i]) return OrbUse::Ignored;      // no click, no orb
    if (!g_ready[i]) return OrbUse::Unsupported; // held from before the run: can't tell if GD would use it
    auto effect = findOrb(orbKey(orb, ghost, ghost->m_isUpsideDown));
    if (!effect) return OrbUse::Unsupported; // never seen this orb in this mode yet
    g_used.push_back(orb);
    g_info.orb = true;
    g_ready[i] = false;
    if (effect->flips) ghost->flipGravity(!ghost->m_isUpsideDown, true);
    ghost->m_yVelocity = effect->velocity;
    ghost->m_isOnGround = false;
    return OrbUse::Used;
}

void learnOrb(PlayerObject* p, GameObject* orb, double velocityBefore, bool flippedBefore) {
    if (!p || !orb || !supportedOrb(orb)) return;
    bool flips = p->m_isUpsideDown != flippedBefore;
    if (!flips && std::abs(p->m_yVelocity - velocityBefore) < 1e-6) return; // GD didn't use it after all
    auto key = orbKey(orb, p, flippedBefore);
    auto known = findOrb(key);
    if (known && std::abs(known->velocity - p->m_yVelocity) < 1e-6 && known->flips == flips) return;
    g_orbs[key] = { p->m_yVelocity, flips };
    Mod::get()->setSavedValue<double>(key + "-v", p->m_yVelocity);
    Mod::get()->setSavedValue<bool>(key + "-f", flips);
}

void track(GameObject* object, double clock) {
    if (!object) return;
    g_lastClock = std::max(g_lastClock, clock);
    // forget objects not seen around the player for a while (long attempts would pile them up)
    static double pruned = 0.0;
    if (g_lastClock - pruned > 2.0 || g_lastClock < pruned) {
        pruned = g_lastClock;
        for (auto it = g_tracks.begin(); it != g_tracks.end();) {
            if (it->second.clock < g_lastClock - 2.0) it = g_tracks.erase(it);
            else ++it;
        }
    }
    auto& t = g_tracks[object];
    double x = object->m_positionX;
    double y = object->m_positionY;
    if (t.clock >= 0.0 && clock > t.clock + 1e-6) {
        double span = clock - t.clock;
        double dx = x - t.x;
        double dy = y - t.y;
        // a jump of more than a block between two looks is a teleport, not movement
        if (std::abs(dx) > 30.0 || std::abs(dy) > 30.0 || span > 2.0) {
            t.vx = t.vy = 0.0;
        }
        else {
            t.vx = dx / span;
            t.vy = dy / span;
        }
    }
    else if (t.clock >= 0.0) {
        return; // looked at it already this step
    }
    t.x = x;
    t.y = y;
    t.clock = clock;
}

void clearTracks() {
    g_tracks.clear();
    g_lastClock = 0.0;
}

int leftoverActions() {
    int n = 0;
    auto am = CCDirector::get()->getActionManager();
    std::function<void(CCNode*, int)> count = [&](CCNode* node, int depth) {
        if (!node || depth > 6) return;
        n += static_cast<int>(am->numberOfRunningActionsInTarget(node));
        if (auto kids = node->getChildren()) {
            for (auto kid : CCArrayExt<CCNode*>(kids)) count(kid, depth + 1);
        }
    };
    count(g_ghost[0], 0);
    count(g_ghost[1], 0);
    return n;
}

bool snapshotsReady() {
    return g_shelf != nullptr;
}

void takeSnapshot(PlayerObject* real, long long step, bool held, size_t inputs, float dt) {
    if (!g_shelf || !real) return;
    int i = g_snapNext;
    g_snapNext = (g_snapNext + 1) % SNAPSHOTS;
    auto& s = g_snaps[i];
    if (!s.player) return;
    mirror(s.player, real);
    quiet(s.player);
    s.step = step;
    s.held = held;
    s.inputs = inputs;
    s.dt = dt;
    g_snapValid[i] = true;
}

Snapshot const* snapshotBefore(long long step) {
    Snapshot const* best = nullptr;
    for (int i = 0; i < SNAPSHOTS; ++i) {
        if (!g_snapValid[i] || g_snaps[i].step > step) continue;
        if (!best || g_snaps[i].step > best->step) best = &g_snaps[i];
    }
    return best;
}

void clearSnapshots() {
    for (auto& v : g_snapValid) v = false;
}

} // namespace pb::ghost
