#pragma once

#include "Buffer.hpp"

namespace pb {

// The on-screen "how early was that" popup.
class BufferOverlay : public cocos2d::CCNode {
public:
    static BufferOverlay* create();
    static BufferOverlay* get(PlayLayer* layer);

    void showBuffered(double earlyMs, int ticks, Mode mode, bool release);
    void showToggle(bool enabled);

protected:
    bool init() override;
    void popup(std::string const& text, cocos2d::ccColor3B color);

    cocos2d::CCLabelBMFont* m_label = nullptr;
};

} // namespace pb
