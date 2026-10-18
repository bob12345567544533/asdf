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
    // Labels are created on demand with their real text (see setLabel), so the
    // popup and the counter never depend on each other.
    this->refresh();
    return true;
}

// Replaces a label with a fresh one showing `text`.
CCLabelBMFont* BufferOverlay::setLabel(CCLabelBMFont*& label, std::string const& text) {
    if (label) {
        label->removeFromParent();
        label = nullptr;
    }
    label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
    this->addChild(label);
    return label;
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

    // The counter sits in the corner, the popup one row in from it.
    if (m_stats) {
        m_stats->setScale(scale);
        m_stats->setAnchorPoint(anchor);
        m_stats->setPosition({ x, y });
    }
    if (m_feedback) {
        m_feedback->setScale(scale);
        m_feedback->setAnchorPoint(anchor);
        m_feedback->setPosition({ x, y + lineHeight * dir });
    }
}

void BufferOverlay::refresh() {
    bool showStats = Mod::get()->getSettingValue<bool>("show-stats");
    if (!showStats) {
        if (m_stats) {
            m_stats->removeFromParent();
            m_stats = nullptr;
        }
    }
    else {
        auto const& s = g.stats;
        double avg = s.sessionCount > 0 ? s.sessionEarlyMs / s.sessionCount : 0.0;
        bool on = Mod::get()->getSettingValue<bool>("enabled");
        auto assist = Mod::get()->getSettingValue<int64_t>("ship-assist");
        auto label = this->setLabel(m_stats, fmt::format(
            "Buffer {} | attempt {} | session {} (avg {:.1f}ms){}",
            on ? "ON" : "OFF", s.attemptCount, s.sessionCount, avg,
            on && assist > 0 ? fmt::format(" | ship assist {}%", assist) : std::string()
        ));
        label->setOpacity(static_cast<GLubyte>(Mod::get()->getSettingValue<int64_t>("label-opacity")));
    }
    this->layout();
}

void BufferOverlay::popup(std::string const& text, ccColor3B color) {
    auto label = this->setLabel(m_feedback, text);
    label->setColor(color);
    label->setOpacity(static_cast<GLubyte>(Mod::get()->getSettingValue<int64_t>("label-opacity")));
    label->runAction(CCSequence::create(
        CCDelayTime::create(1.0f),
        CCFadeOut::create(0.4f),
        nullptr
    ));
    this->layout();
}

void BufferOverlay::showBuffered(double earlyMs, int ticks, Mode mode, bool release) {
    this->refresh();
    if (!Mod::get()->getSettingValue<bool>("show-feedback")) return;
    this->popup(fmt::format(
        "{} {:.1f}ms early ({} tick{}, {})",
        release ? "Release" : "Click", earlyMs, ticks, ticks == 1 ? "" : "s", modeName(mode)
    ), { 120, 255, 140 });
}

void BufferOverlay::showToggle(bool enabled) {
    this->refresh();
    this->popup(enabled ? "Buffer ON" : "Buffer OFF", enabled ? ccColor3B { 120, 255, 140 } : ccColor3B { 255, 120, 120 });
}

} // namespace pb
