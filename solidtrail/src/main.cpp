// Solid Wave Trail: GD draws the wave trail with additive blending (it glows
// and lets the level show through). This keeps it in GD's own solid mode with
// normal blending. It's re-checked every time the trail updates, so nothing that resets
// the trail (respawns, mode changes, other mods) can switch it back.

#include <Geode/Geode.hpp>
#include <Geode/modify/HardStreak.hpp>

using namespace geode::prelude;

namespace {

bool g_enabled = true;

constexpr ccBlendFunc SOLID { GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA };

bool same(ccBlendFunc const& a, ccBlendFunc const& b) {
    return a.src == b.src && a.dst == b.dst;
}

} // namespace

class $modify(SolidHardStreak, HardStreak) {
    void updateStroke(float dt) {
        if (!g_enabled) return HardStreak::updateStroke(dt); // off: leave the trail to GD (and other mods)
        // GD's own solid mode for the trail (GD turns it back off over time), set
        // before the trail is rebuilt so this frame is already drawn solid
        m_isSolid = true;
        HardStreak::updateStroke(dt);
        // a plain compare most frames; only writes when something changed it
        if (!same(this->getBlendFunc(), SOLID)) this->setBlendFunc(SOLID);
    }
};

$on_mod(Loaded) {
    g_enabled = Mod::get()->getSettingValue<bool>("enabled");
    listenForSettingChanges<bool>("enabled", [](bool value) { g_enabled = value; });
}
