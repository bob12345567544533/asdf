#include "Ghost.hpp"

#include "Config.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
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
std::vector<GameObject*> g_used; // pads / portals the ghost already used this run

// How hard GD bumps the player for each pad type, learned from the real player
// (so the ghost bumps exactly like the game does). Saved between sessions.
std::unordered_map<int, float> g_bump;
bool g_bumpLoaded = false;

// What each orb does to the player, learned from the real player: the velocity it
// leaves you with and whether it flips gravity, per orb type / mode / size / gravity.
struct OrbEffect {
    double velocity = 0.0;
    bool flips = false;
};
std::unordered_map<std::string, OrbEffect> g_orbs;

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

// Fields copyAttributes leaves out (GD 2.2081 Windows; same set the original
// mod copied): slope / ground state and a few movement values.
template <class T>
void copyRaw(PlayerObject* to, PlayerObject* from, size_t offset) {
    std::memcpy(reinterpret_cast<char*>(to) + offset, reinterpret_cast<char const*>(from) + offset, sizeof(T));
}

void mirror(PlayerObject* ghost, PlayerObject* real) {
    ghost->setVisible(false);
    ghost->copyAttributes(real);
    ghost->setScale(real->getScale());
    ghost->setContentSize(real->getContentSize());
    copyRaw<float>(ghost, real, 0x2fc);
    copyRaw<float>(ghost, real, 0x300);
    copyRaw<float>(ghost, real, 0xb84);
    copyRaw<bool>(ghost, real, 0x9c1);
    copyRaw<bool>(ghost, real, 0x9bf);
    copyRaw<float>(ghost, real, 0x9f0);
    copyRaw<float>(ghost, real, 0x9f4);
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

void loadBumps() {
    if (g_bumpLoaded) return;
    g_bumpLoaded = true;
    auto mod = Mod::get();
    for (auto type : { GameObjectType::YellowJumpPad, GameObjectType::PinkJumpPad, GameObjectType::RedJumpPad }) {
        auto key = fmt::format("bump-{}", static_cast<int>(type));
        if (mod->hasSavedValue(key)) g_bump[static_cast<int>(type)] = mod->getSavedValue<float>(key);
    }
}

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

    // Positions `steps` after the input arrived.
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

// Ends a run and gives back its result.
struct RunGuard {
    RunGuard() {
        g_running = true;
        g_dead = false;
        g_stopped = false;
        g_held[0] = g_held[1] = false;
        g_ready[0] = g_ready[1] = false;
        g_used.clear();
    }
    ~RunGuard() { g_running = false; }
};

} // namespace

void create(PlayLayer* layer) {
    destroy();
    clearTracks();
    loadBumps();
    if (!layer || !layer->m_objectLayer) return;
    g_ghost[0] = make(layer);
    g_ghost[1] = make(layer);
}

void destroy() {
    // Just let go: the level owns the node and cleans it up with everything else
    // (taking it out by hand runs PlayerObject's exit code, best left alone).
    for (auto& ghost : g_ghost) {
        if (!ghost) continue;
        ghost->release();
        ghost = nullptr;
    }
    g_running = false;
    clearTracks();
}

bool ready() {
    return g_ghost[0] != nullptr;
}

Result run(PlayLayer* layer, Setup const& s, double delay, bool down, int horizon) {
    Result result;
    if (!layer || !s.p1 || !g_ghost[0]) {
        result.survived = horizon;
        return result;
    }
    bool dual = s.p2 && g_ghost[1];
    int count = dual ? 2 : 1;
    PlayerObject* real[2] { s.p1, s.p2 };

    bool now = delay < 0.0;
    int inputStep = now ? -1 : static_cast<int>(std::floor(delay + 1e-6));
    float frac = now ? 0.f : static_cast<float>(delay - inputStep);
    if (frac < 0.01f) frac = 0.f;
    float rem = std::clamp(s.remainder, 0.f, 0.999f);
    if (rem < 0.01f) rem = 0.f;

    RunGuard guard;
    Movers movers(config().movingObjects, s.dt);
    for (int i = 0; i < count; ++i) {
        if (s.fresh) freshen(g_ghost[i]);
        mirror(g_ghost[i], real[i]);
        if (now) press(i, down);
        else if (delay > 0.0 || rem > 0.f) press(i, !down); // the button as it is now, until the input
    }
    bool applied = now;
    double t = 0.0; // steps since the input arrived (for moving objects)

    // per player: cube-like modes count a jump that lands and stays down as safe
    int grounded[2] {};
    int airborne[2] {};
    bool clear[2] {};
    int sincePress = 0;
    auto afterInputStep = [&]() -> bool { // true = the run is over
        result.survived++;
        // your usual tap: let go when you normally would
        if (down && s.tapSteps > 0 && ++sincePress == s.tapSteps) {
            for (int i = 0; i < count; ++i) press(i, false);
        }
        if (result.survived >= horizon) return true;
        if (s.flying) return false;
        bool allClear = true;
        for (int i = 0; i < count; ++i) {
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
    // collisions at the end of a step; false = the run ended (death / something it can't use)
    auto collide = [&](bool beforeInput) {
        for (int i = 0; i < count; ++i) {
            clearLogs(g_ghost[i]);
            layer->checkCollisions(g_ghost[i], s.dt, false);
            if (g_dead) {
                result.diedBeforeInput = beforeInput;
                return false;
            }
        }
        if (g_stopped) {
            result.hitStop = true;
            return false;
        }
        return true;
    };

    // the rest of the step your input arrived in
    if (rem > 0.f) {
        t += rem;
        movers.place(t);
        for (int i = 0; i < count; ++i) {
            g_ghost[i]->update(s.dt * rem);
            g_ghost[i]->updateRotation(s.dt * rem);
        }
        if (g_dead) {
            result.diedBeforeInput = !applied;
            return result;
        }
        if (!collide(!applied)) return result;
        if (applied && afterInputStep()) return result;
    }

    bool pressNext = false; // the split wasn't possible: the input goes in at the start of the next step
    int cap = horizon + std::max(inputStep, 0) + 2;
    for (int step = 0; step < cap; ++step) {
        t += 1.0;
        movers.place(t);
        bool pressing = !applied && (pressNext || (step == inputStep && frac == 0.f));
        bool splitting = !applied && !pressing && step == inputStep && frac > 0.f;
        if (splitting && (dual || !splittable(g_ghost[0]))) {
            splitting = false;
            pressNext = true; // same fallback as the real game: whole step now, input at the next one
        }
        for (int i = 0; i < count; ++i) {
            if (pressing) press(i, down);
            if (splitting) {
                bool startedOnGround = g_ghost[i]->m_isOnGround;
                g_ghost[i]->update(s.dt * frac);
                splitCollide(layer, g_ghost[i], startedOnGround, s.dt);
                press(i, down);
                g_ghost[i]->update(s.dt * (1.f - frac));
            }
            else {
                g_ghost[i]->update(s.dt);
            }
            g_ghost[i]->updateRotation(s.dt);
        }
        if (pressing || splitting) {
            applied = true;
            pressNext = false;
        }
        if (g_dead) {
            result.diedBeforeInput = !applied;
            return result;
        }
        if (!collide(!applied)) return result;
        if (applied && afterInputStep()) return result;
    }
    return result;
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
    auto it = g_bump.find(static_cast<int>(pad->m_objectType));
    if (it == g_bump.end()) return false; // never seen GD use this pad type yet
    g_used.push_back(pad);
    ghost->bumpPlayer(it->second, static_cast<int>(pad->m_objectType), true, pad);
    return true;
}

bool useSpeedPortal(PlayerObject* ghost, GameObject* portal, float speed) {
    if (used(portal)) return true;
    g_used.push_back(portal);
    ghost->m_playerSpeed = speed;
    return true;
}

bool useGravityPortal(PlayerObject* ghost, GameObject* portal) {
    if (used(portal)) return true;
    g_used.push_back(portal);
    bool flip = ghost->m_isUpsideDown;
    switch (portal->m_objectType) {
        case GameObjectType::InverseGravityPortal: flip = true; break;
        case GameObjectType::NormalGravityPortal: flip = false; break;
        case GameObjectType::GravityTogglePortal: flip = !ghost->m_isUpsideDown; break;
        default: return false;
    }
    if (flip != ghost->m_isUpsideDown) ghost->flipGravity(flip, true);
    return true;
}

OrbUse useOrb(PlayerObject* ghost, GameObject* orb) {
    if (!supportedOrb(orb)) return OrbUse::Unsupported;
    int i = ghost == g_ghost[1] ? 1 : 0;
    if (used(orb)) return OrbUse::Ignored;
    if (!g_held[i]) return OrbUse::Ignored;      // no click, no orb
    if (!g_ready[i]) return OrbUse::Unsupported; // held from before the check: can't tell if GD would use it
    auto effect = findOrb(orbKey(orb, ghost, ghost->m_isUpsideDown));
    if (!effect) return OrbUse::Unsupported; // never seen this orb in this mode yet
    g_used.push_back(orb);
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

void learnBump(int objectType, float bumpMod) {
    loadBumps();
    switch (static_cast<GameObjectType>(objectType)) {
        case GameObjectType::YellowJumpPad:
        case GameObjectType::PinkJumpPad:
        case GameObjectType::RedJumpPad:
            break;
        default:
            return;
    }
    auto it = g_bump.find(objectType);
    if (it != g_bump.end() && std::abs(it->second - bumpMod) < 1e-4f) return;
    g_bump[objectType] = bumpMod;
    Mod::get()->setSavedValue<float>(fmt::format("bump-{}", objectType), bumpMod);
}

void track(GameObject* object, double clock) {
    if (!object) return;
    g_lastClock = std::max(g_lastClock, clock);
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

} // namespace pb::ghost
