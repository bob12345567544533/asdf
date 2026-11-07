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
std::vector<GameObject*> g_used; // pads / portals the ghost already used this run

// How hard GD bumps the player for each pad type, learned from the real player
// (so the ghost bumps exactly like the game does). Saved between sessions.
std::unordered_map<int, float> g_bump;
bool g_bumpLoaded = false;

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

    // Positions for ghost step `step` (0 = the first step after the input arrives).
    void place(int step) {
        double t = (step + 1) * static_cast<double>(m_dt);
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

    delay = std::max(0.0, delay);
    int inputStep = static_cast<int>(std::floor(delay + 1e-6));
    float frac = static_cast<float>(delay - inputStep);
    if (frac < 0.01f) frac = 0.f;

    RunGuard guard;
    Movers movers(config().movingObjects, s.dt);
    for (int i = 0; i < count; ++i) {
        if (s.fresh) freshen(g_ghost[i]);
        mirror(g_ghost[i], real[i]);
        if (delay > 0.0) press(i, !down); // the button as it is now, until the input
    }

    int total = horizon + inputStep;
    // per player: cube-like modes count a jump that lands and stays down as safe
    int grounded[2] {};
    int airborne[2] {};
    bool clear[2] {};
    for (int step = 0; step < total; ++step) {
        bool beforeInput = step < inputStep;
        movers.place(step);

        for (int i = 0; i < count; ++i) {
            if (step == inputStep && frac > 0.f) {
                // the input lands `frac` of the way into this step
                g_ghost[i]->update(s.dt * frac);
                press(i, down);
                g_ghost[i]->update(s.dt * (1.f - frac));
            }
            else {
                if (step == inputStep) press(i, down);
                g_ghost[i]->update(s.dt);
            }
            g_ghost[i]->updateRotation(s.dt);
        }
        if (g_dead) {
            result.diedBeforeInput = beforeInput;
            return result;
        }
        for (int i = 0; i < count; ++i) {
            clearLogs(g_ghost[i]);
            layer->checkCollisions(g_ghost[i], s.dt, false);
            if (g_dead) {
                result.diedBeforeInput = beforeInput;
                return result;
            }
        }
        if (g_stopped) {
            result.hitStop = true;
            return result;
        }

        if (beforeInput) continue;
        result.survived++;
        if (s.flying) continue;

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
            return result;
        }
    }
    return result;
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
