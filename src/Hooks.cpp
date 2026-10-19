#include "Buffer.hpp"
#include "Menu.hpp"
#include "Overlay.hpp"

#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Level lifecycle: ghost players, overlay, cleanup
// ---------------------------------------------------------------------------

class $modify(PBPlayLayer, PlayLayer) {
    struct Fields {
        Ref<PlayerObject> ghost1;
        Ref<PlayerObject> ghost2;
        ~Fields() {
            pb::clearGhosts(ghost1.data(), ghost2.data());
        }
    };

    PlayerObject* makeGhost() {
        auto ghost = PlayerObject::create(1, 1, this, this, true);
        ghost->setPosition({ 0.f, 105.f });
        ghost->setVisible(false);
        m_objectLayer->addChild(ghost);
        return ghost;
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        pb::clearQueues();
        pb::resetTiming();

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        m_fields->ghost1 = this->makeGhost();
        m_fields->ghost2 = this->makeGhost();
        pb::setGhosts(m_fields->ghost1.data(), m_fields->ghost2.data());

        if (auto overlay = pb::BufferOverlay::create()) {
            this->addChild(overlay, 10000);
        }
        pb::menu::attempt(m_attempts, true);
        return true;
    }

    void resetLevel() {
        pb::clearQueues();
        pb::resetTiming();
        PlayLayer::resetLevel();
        pb::menu::attempt(m_attempts);
    }

    void onQuit() {
        pb::clearQueues();
        pb::clearGhosts(m_fields->ghost1.data(), m_fields->ghost2.data());
        PlayLayer::onQuit();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (pb::g_simulating && pb::isGhost(player)) {
            // A ghost hit something. Note it and stop: no death effects, no reset.
            // Mark it dead like a real death would, so GD's own checks after
            // this call don't see a player that hit something and lived.
            pb::g_simDied = true;
            player->m_isDead = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);

        // Only drop held inputs if you actually died (noclip keeps you alive).
        if (player && player->m_isDead) {
            pb::clearQueues();
            int percent = -1;
            if (m_player1 && m_levelLength > 0.f) {
                percent = static_cast<int>(std::clamp(m_player1->getPositionX() / m_levelLength * 100.f, 0.f, 100.f));
            }
            pb::menu::death(percent);
        }
    }

    void playEndAnimationToPos(CCPoint pos) {
        if (pb::g_simulating) return;
        PlayLayer::playEndAnimationToPos(pos);
    }
};

// ---------------------------------------------------------------------------
// Input interception and side-effect blocking for the ghosts
// ---------------------------------------------------------------------------

class $modify(PBBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        if (pl && static_cast<GJBaseGameLayer*>(pl) == this) {
            if (pb::onButton(pl, down, button, isPlayer1)) return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // Ghosts only collide with blocks, slopes and hazards. Touching a portal,
    // pad or orb (while holding) ends the simulation instead of being ignored,
    // because the ghost can't use them without changing the real level.
    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int count, float dt) {
        if (!pb::g_simulating || !objects) {
            return GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);
        }

        // GD's own collision pass is never used for ghosts: it records every
        // block and spike a ghost touches in the level's bookkeeping for the
        // real player, and then won't save your PB. Instead the ghost reacts
        // to blocks and slopes through its own player physics, spikes are
        // checked by hand, and GD's pass only gets an empty list.
        std::vector<GameObject*> solids;
        std::vector<GameObject*> slopes;
        std::vector<GameObject*> stops;
        std::vector<GameObject*> hazards;

        int n = std::min<int>(count, static_cast<int>(objects->size()));
        for (int i = 0; i < n; ++i) {
            auto obj = (*objects)[i];
            if (!obj || obj->m_isGroupDisabled) continue;
            switch (obj->m_objectType) {
                case GameObjectType::Hazard:
                case GameObjectType::AnimatedHazard:
                    hazards.push_back(obj);
                    break;
                case GameObjectType::Solid:
                    solids.push_back(obj);
                    break;
                case GameObjectType::Slope:
                    slopes.push_back(obj);
                    break;
                case GameObjectType::CollisionObject:
                    break;
                default:
                    // Portals/pads/orbs: checked below, from cached hitboxes.
                    if (pb::stopsSimulation(obj, player)) stops.push_back(obj);
                    break;
            }
        }

        static gd::vector<GameObject*> empty;
        empty.clear();
        GJBaseGameLayer::collisionCheckObjects(player, &empty, 0, dt);

        // Slopes can't be modeled without GD's collision code: unknown, not a death.
        for (auto obj : slopes) {
            if (player->getObjectRect().intersectsRect(obj->getObjectRect())) {
                pb::g_simStopped = true;
                return;
            }
        }
        // Blocks, by plain math: coming down onto a block lands on it, coming
        // up into one lands on its underside (modes that can ride ceilings),
        // anything else is a crash. Nothing here calls GD with the block.
        int gi = pb::ghostIndex(player);
        float prevY = gi >= 0 ? pb::g_prevPos[gi].y : player->getPositionY();
        bool flipped = player->m_isUpsideDown;
        bool ridesCeiling = player->m_isShip || player->m_isBird || player->m_isBall
            || player->m_isDart || player->m_isSwing || player->m_isSpider;
        for (auto obj : solids) {
            CCRect me = player->getObjectRect();
            CCRect box = obj->getObjectRect();
            if (!me.intersectsRect(box)) continue;
            float half = me.size.height / 2.f;
            float vy = player->m_yVelocity * (flipped ? -1.f : 1.f); // >0 = moving away from "ground"
            bool fromAbove = prevY - half >= box.getMaxY() - 2.f;
            bool fromBelow = prevY + half <= box.getMinY() + 2.f;
            bool movingDown = flipped ? player->m_yVelocity >= 0.f : player->m_yVelocity <= 0.f;
            (void)vy;
            if (fromAbove && (flipped ? ridesCeiling : true) && player->m_yVelocity <= 0.f) {
                player->setPosition({ player->getPositionX(), box.getMaxY() + half });
                player->m_yVelocity = 0.f;
                if (!flipped) player->m_isOnGround = true;
            }
            else if (fromBelow && (flipped ? true : ridesCeiling) && player->m_yVelocity >= 0.f) {
                player->setPosition({ player->getPositionX(), box.getMinY() - half });
                player->m_yVelocity = 0.f;
                if (flipped) player->m_isOnGround = true;
            }
            else {
                (void)movingDown;
                pb::g_simDied = true;
                return;
            }
        }

        CCRect ghostBox = player->getObjectRect();
        for (auto obj : hazards) {
            CCRect box = obj->getObjectRect();
            bool hit;
            if (obj->m_objectRadius > 0.f) {
                // Round hitbox (saws): closest point of the ghost's box to the center.
                float cx = box.getMidX(), cy = box.getMidY();
                float px = std::clamp(cx, ghostBox.getMinX(), ghostBox.getMaxX());
                float py = std::clamp(cy, ghostBox.getMinY(), ghostBox.getMaxY());
                float r = obj->m_objectRadius;
                hit = (px - cx) * (px - cx) + (py - cy) * (py - cy) <= r * r;
            }
            else {
                hit = ghostBox.intersectsRect(box);
            }
            if (hit) {
                pb::g_simDied = true;
                return;
            }
        }
        for (auto obj : stops) {
            if (ghostBox.intersectsRect(obj->m_objectRect)) {
                pb::g_simStopped = true;
                break;
            }
        }
    }

    // Ghost landings/jumps must not fire the level's event triggers.
    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (pb::g_simulating) return;
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (pb::g_simulating) return false;
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (pb::g_simulating) return;
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }
};

class $modify(PBPlayerObject, PlayerObject) {
    void update(float dt) {
        if (!pb::g_simulating) {
            auto pl = PlayLayer::get();
            if (pl && this == pl->m_player1) pb::onStep(pl, dt);
        }
        PlayerObject::update(dt);
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        if (pb::g_simulating) return;
        PlayerObject::ringJump(ring, skipCheck);
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (pb::g_simulating) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void incrementJumps() {
        if (pb::g_simulating) return;
        PlayerObject::incrementJumps();
    }
};

class $modify(EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (pb::g_simulating) return;
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(GameObject) {
    void playShineEffect() {
        if (pb::g_simulating) return;
        GameObject::playShineEffect();
    }
};

class $modify(HardStreak) {
    void addPoint(CCPoint point) {
        if (pb::g_simulating) return;
        HardStreak::addPoint(point);
    }
};

// ---------------------------------------------------------------------------
// Keybinds and live settings
// ---------------------------------------------------------------------------

$on_mod(Loaded) {
    listenForKeybindSettingPresses("toggle-key", [](Keybind const&, bool down, bool repeat, double) {
        if (!down || repeat) return;

        bool enabled = !Mod::get()->getSettingValue<bool>("enabled");
        Mod::get()->setSettingValue<bool>("enabled", enabled);

        auto pl = PlayLayer::get();
        if (!enabled && pl) {
            // Don't swallow anything that was waiting to fire.
            pb::flushChannel(pl, 0);
            pb::flushChannel(pl, 1);
        }

        pb::menu::log(enabled ? "Buffer ON" : "Buffer OFF");

        if (auto overlay = pb::BufferOverlay::get(pl)) {
            overlay->showToggle(enabled);
        }
        else {
            Notification::create(
                enabled ? "Buffer ON" : "Buffer OFF",
                enabled ? NotificationIcon::Success : NotificationIcon::Info
            )->show();
        }
    });

    // Turning the buffer off any way (F6, the window, Geode settings) fires
    // whatever was being held back right away, in order.
    listenForSettingChanges<bool>("enabled", [](bool enabled) {
        if (enabled) return;
        if (auto pl = PlayLayer::get()) {
            pb::flushChannel(pl, 0);
            pb::flushChannel(pl, 1);
        }
    });

    listenForKeybindSettingPresses("menu-key", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) pb::menu::toggle();
    });

    listenForAllSettingChanges([](std::string_view, std::shared_ptr<SettingV3>) {
        pb::menu::sync();
    });
}

// Open the window when the game starts, like the old console did.
$on_game(Loaded) {
    if (Mod::get()->getSettingValue<bool>("open-window")) pb::menu::open();
    else pb::menu::prepare();
}
