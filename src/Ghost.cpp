#include "Ghost.hpp"

#include <Geode/Geode.hpp>

#include <cstring>

using namespace geode::prelude;

namespace pb::ghost {

namespace {

PlayerObject* g_ghost[2] { nullptr, nullptr };
bool g_running = false;
bool g_dead = false;
bool g_stopped = false;
bool g_held[2] { false, false };

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

// Ends a run and gives back its result.
struct RunGuard {
    RunGuard() {
        g_running = true;
        g_dead = false;
        g_stopped = false;
        g_held[0] = g_held[1] = false;
    }
    ~RunGuard() { g_running = false; }
};

} // namespace

void create(PlayLayer* layer) {
    destroy();
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
}

bool ready() {
    return g_ghost[0] != nullptr;
}

Result run(PlayLayer* layer, Setup const& s, int delay, bool down, int horizon) {
    Result result;
    if (!layer || !s.p1 || !g_ghost[0]) {
        result.survived = horizon;
        return result;
    }
    bool dual = s.p2 && g_ghost[1];
    int count = dual ? 2 : 1;
    PlayerObject* real[2] { s.p1, s.p2 };

    RunGuard guard;
    for (int i = 0; i < count; ++i) {
        if (s.fresh) freshen(g_ghost[i]);
        mirror(g_ghost[i], real[i]);
        if (delay > 0) press(i, !down); // the button as it is now, until the input
    }

    int total = horizon + delay;
    int grounded = 0;
    int airborne = 0;
    for (int step = 0; step < total; ++step) {
        bool beforeInput = step < delay;
        if (step == delay) {
            for (int i = 0; i < count; ++i) press(i, down);
        }

        for (int i = 0; i < count; ++i) {
            g_ghost[i]->update(s.dt);
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

        // Cube-like modes: a jump that lands and stays down for a few steps is safe.
        bool onGround = g_ghost[0]->m_isOnGround && (!dual || g_ghost[1]->m_isOnGround);
        if (!onGround) {
            airborne++;
            grounded = 0;
            continue;
        }
        if (airborne < 6) continue;
        if (grounded > 2) {
            result.survived = horizon;
            result.cleared = true;
            return result;
        }
        grounded++;
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

} // namespace pb::ghost
