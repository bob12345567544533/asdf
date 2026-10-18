#include "Overlay.hpp"

using namespace geode::prelude;

namespace pb {

static constexpr char const* OVERLAY_ID = "input-buffer-overlay";

BufferOverlay* BufferOverlay::create() {
    auto ret = new BufferOverlay();
    if (ret->init()) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

BufferOverlay* BufferOverlay::get(PlayLayer* layer) {
    if (!layer) return nullptr;
    return static_cast<BufferOverlay*>(layer->getChildByID(OVERLAY_ID));
}

bool BufferOverlay::init() {
    if (!CCNode::init()) return false;
    this->setID(OVERLAY_ID);

    m_feedback = CCLabelBMFont::create("", "bigFont.fnt");
    m_feedback->setOpacity(0);
    this->addChild(m_feedback);

    m_stats = CCLabelBMFont::create("", "bigFont.fnt");
    this->addChild(m_stats);

    this->refresh();
    return true;
}

void BufferOverlay::layout() {
    auto win = CCDirector::get()->getWinSize();
    auto pos = Mod::get()->getSettingValue<std::string>("label-position");
    float scale = static_cast<float>(Mod::get()->getSettingValue<double>("label-scale"));

    bool left = pos.find("Left") != std::string::npos;
    bool top = pos.find("Top") != std::string::npos;

    constexpr float margin = 6.f;
    float x = left ? margin : win.width - margin;
    float y = top ? win.height - margin : margin;
    CCPoint anchor { left ? 0.f : 1.f, top ? 1.f : 0.f };

    float lineHeight = 32.f * scale;
    float dir = top ? -1.f : 1.f;

    // The counter sits closest to the corner, the popup goes next to it.
    for (auto label : { m_stats, m_feedback }) {
        label->setScale(scale);
        label->setAnchorPoint(anchor);
    }
    // Fixed rows: the popup always sits one row in from the corner, whether the
    // counter is on or not, so the counter can never hide or move it.
    m_stats->setPosition({ x, y });
    m_feedback->setPosition({ x, y + lineHeight * dir });
}

void BufferOverlay::refresh() {
    bool showStats = Mod::get()->getSettingValue<bool>("show-stats");
    m_stats->setVisible(showStats);
    if (showStats) {
        auto const& s = g.stats;
        double avg = s.sessionCount > 0 ? s.sessionEarlyMs / s.sessionCount : 0.0;
        bool on = Mod::get()->getSettingValue<bool>("enabled");
        auto assist = Mod::get()->getSettingValue<int64_t>("ship-assist");
        m_stats->setString(fmt::format(
            "Buffer {} | attempt {} | session {} (avg {:.1f}ms){}",
            on ? "ON" : "OFF", s.attemptCount, s.sessionCount, avg,
            on && assist > 0 ? fmt::format(" | ship assist {}%", assist) : std::string()
        ).c_str());
        auto opacity = static_cast<GLubyte>(Mod::get()->getSettingValue<int64_t>("label-opacity"));
        m_stats->setOpacity(opacity);
    }
    this->layout();
}

void BufferOverlay::showBuffered(double earlyMs, int ticks, Mode mode, bool release) {
    this->refresh();
    if (!Mod::get()->getSettingValue<bool>("show-feedback")) return;

    m_feedback->setString(fmt::format(
        "{} {:.1f}ms early ({} tick{}, {})",
        release ? "Release" : "Click", earlyMs, ticks, ticks == 1 ? "" : "s", modeName(mode)
    ).c_str());
    m_feedback->setColor({ 120, 255, 140 });

    auto opacity = static_cast<GLubyte>(Mod::get()->getSettingValue<int64_t>("label-opacity"));
    m_feedback->stopAllActions();
    m_feedback->setOpacity(opacity);
    m_feedback->runAction(CCSequence::create(
        CCDelayTime::create(1.0f),
        CCFadeOut::create(0.4f),
        nullptr
    ));
    this->layout();
}

void BufferOverlay::showToggle(bool enabled) {
    this->refresh();
    m_feedback->setString(enabled ? "Buffer ON" : "Buffer OFF");
    m_feedback->setColor(enabled ? ccColor3B { 120, 255, 140 } : ccColor3B { 255, 120, 120 });

    auto opacity = static_cast<GLubyte>(Mod::get()->getSettingValue<int64_t>("label-opacity"));
    m_feedback->stopAllActions();
    m_feedback->setOpacity(opacity);
    m_feedback->runAction(CCSequence::create(
        CCDelayTime::create(1.0f),
        CCFadeOut::create(0.4f),
        nullptr
    ));
    this->layout();
}

} // namespace pb
