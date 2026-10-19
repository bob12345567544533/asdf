#include "Overlay.hpp"

using namespace geode::prelude;

namespace pb {

static constexpr char const* OVERLAY_ID = "input-buffer-lite-overlay";

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
    return true;
}

// Builds a fresh label with the real text every time (no reused empty labels).
void BufferOverlay::popup(std::string const& text, ccColor3B color) {
    if (m_label) {
        m_label->removeFromParent();
        m_label = nullptr;
    }

    auto win = CCDirector::get()->getWinSize();
    auto pos = Mod::get()->getSettingValue<std::string>("label-position");
    float scale = static_cast<float>(Mod::get()->getSettingValue<double>("label-scale"));
    bool left = pos.find("Left") != std::string::npos;
    bool top = pos.find("Top") != std::string::npos;
    constexpr float margin = 6.f;

    m_label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
    m_label->setScale(scale);
    m_label->setAnchorPoint({ left ? 0.f : 1.f, top ? 1.f : 0.f });
    m_label->setPosition({ left ? margin : win.width - margin, top ? win.height - margin : margin });
    m_label->setColor(color);
    m_label->setOpacity(static_cast<GLubyte>(Mod::get()->getSettingValue<int64_t>("label-opacity")));
    this->addChild(m_label);

    m_label->runAction(CCSequence::create(
        CCDelayTime::create(1.0f),
        CCFadeOut::create(0.4f),
        nullptr
    ));
}

void BufferOverlay::showBuffered(double earlyMs, int ticks, Mode mode, bool release, bool random) {
    if (!Mod::get()->getSettingValue<bool>("show-feedback")) return;
    this->popup(fmt::format(
        "{} {:.1f}ms early ({} tick{}, {}{})",
        release ? "Release" : "Click", earlyMs, ticks, ticks == 1 ? "" : "s", modeName(mode),
        random ? ", random" : ""
    ), { 120, 255, 140 });
}

void BufferOverlay::showToggle(bool enabled) {
    this->popup(enabled ? "Buffer ON" : "Buffer OFF", enabled ? ccColor3B { 120, 255, 140 } : ccColor3B { 255, 120, 120 });
}

} // namespace pb
